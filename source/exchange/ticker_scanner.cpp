#include "exchange/ticker_scanner.hpp"

#include "core/logger.hpp"
#include "engine/orderbook.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>

namespace kalshi {

namespace {

constexpr double kSecondsPerDay = 86400.0;
constexpr double kCentiCentsPerDollar = 10000.0;

constexpr std::size_t kTickerDateTokenChars = 7;
constexpr std::array<std::string_view, 12> kTickerMonthTokens = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
constexpr std::size_t kYearTensIndex = 0;
constexpr std::size_t kYearOnesIndex = 1;
constexpr std::size_t kMonthTokenOffset = 2;
constexpr std::size_t kMonthTokenChars = 3;
constexpr std::size_t kDayTensIndex = 5;
constexpr std::size_t kDayOnesIndex = 6;
constexpr int kTickerBaseYear = 2000;
constexpr int kYearKeyFactor = 10'000;
constexpr int kMonthKeyFactor = 100;
constexpr int kDecimalBase = 10;
constexpr int kMaxDayOfMonth = 31;

bool is_ascii_digit(char character) {
  return character >= '0' && character <= '9';
}

std::optional<int> ticker_event_date_key(std::string_view ticker) {
  const auto dash = ticker.find('-');
  if (dash == std::string_view::npos ||
      ticker.size() < dash + 1 + kTickerDateTokenChars) {
    return std::nullopt;
  }
  const auto token = ticker.substr(dash + 1, kTickerDateTokenChars);
  if (!is_ascii_digit(token[kYearTensIndex]) ||
      !is_ascii_digit(token[kYearOnesIndex]) ||
      !is_ascii_digit(token[kDayTensIndex]) ||
      !is_ascii_digit(token[kDayOnesIndex])) {
    return std::nullopt;
  }
  const auto month_token = token.substr(kMonthTokenOffset, kMonthTokenChars);
  const auto *const month_iter =
      std::ranges::find(kTickerMonthTokens, month_token);
  if (month_iter == kTickerMonthTokens.end()) {
    return std::nullopt;
  }
  const int month =
      static_cast<int>(std::distance(kTickerMonthTokens.begin(), month_iter)) +
      1;
  const int year = kTickerBaseYear +
                   ((token[kYearTensIndex] - '0') * kDecimalBase) +
                   (token[kYearOnesIndex] - '0');
  const int day = ((token[kDayTensIndex] - '0') * kDecimalBase) +
                  (token[kDayOnesIndex] - '0');
  if (day < 1 || day > kMaxDayOfMonth) {
    return std::nullopt;
  }
  return (year * kYearKeyFactor) + (month * kMonthKeyFactor) + day;
}

int utc_date_key(std::chrono::system_clock::time_point now) {
  const std::chrono::year_month_day date{
      std::chrono::floor<std::chrono::days>(now)};
  return (static_cast<int>(date.year()) * kYearKeyFactor) +
         (static_cast<int>(static_cast<unsigned>(date.month())) *
          kMonthKeyFactor) +
         static_cast<int>(static_cast<unsigned>(date.day()));
}

constexpr double kWeightVolume = 0.70;
constexpr double kWeightSpread = 0.30;

// Spread scoring peaks at the midpoint of the configured
// [min_spread, max_spread] filter range and falls to zero at the edges.
struct SpreadCurve {
  double mid_cents;
  double half_range_cents;
};

SpreadCurve spread_curve(const ScannerConfig &config) {
  constexpr double kMinHalfRange = 0.5;
  const double mid = (config.min_spread_cents + config.max_spread_cents) / 2.0;
  const double half_range = std::max(
      kMinHalfRange, (config.max_spread_cents - config.min_spread_cents) / 2.0);
  return {.mid_cents = mid, .half_range_cents = half_range};
}

double compute_score(const MarketScore &market, double max_volume,
                     const SpreadCurve &curve) {
  double vol_term = 0.0;
  if (max_volume > 1.0 && market.volume_24h > 1.0) {
    vol_term = std::log(market.volume_24h) / std::log(max_volume);
  }

  const double spread_term =
      1.0 -
      (std::abs(static_cast<double>(market.spread_cents) - curve.mid_cents) /
       curve.half_range_cents);

  return (kWeightVolume * vol_term) +
         (kWeightSpread * std::max(0.0, spread_term));
}

double incentive_term(const MarketScore &market, double max_reward) {
  if (market.incentive_reward_dollars <= 0.0 || max_reward <= 1.0) {
    return 0.0;
  }
  return std::log(market.incentive_reward_dollars + 1.0) /
         std::log(max_reward + 1.0);
}

double join_incentives(std::vector<MarketScore> &candidates,
                       const std::vector<IncentiveProgram> &programs) {
  std::unordered_map<std::string, double> reward_by_ticker;
  for (const auto &program : programs) {
    reward_by_ticker[program.market_ticker] =
        static_cast<double>(program.period_reward_centicents) /
        kCentiCentsPerDollar;
  }

  double max_reward = 0.0;
  for (auto &candidate : candidates) {
    const auto reward = reward_by_ticker.find(candidate.ticker);
    if (reward != reward_by_ticker.end()) {
      candidate.incentive_reward_dollars = reward->second;
      max_reward = std::max(max_reward, reward->second);
    }
  }
  return max_reward;
}

std::vector<Market> fetch_markets(RestClient &rest,
                                  const std::vector<std::string> &series) {
  if (series.empty()) {
    return rest.get_markets();
  }
  std::vector<Market> all;
  for (const auto &event : series) {
    auto batch = rest.get_markets(event);
    all.insert(all.end(), std::make_move_iterator(batch.begin()),
               std::make_move_iterator(batch.end()));
  }
  return all;
}

} // namespace

TickerScanner::TickerScanner(RestClient &rest, ScannerConfig config)
    : rest_{rest}, config_{std::move(config)} {}

std::vector<MarketScore>
TickerScanner::scan(int top_n,
                    std::chrono::system_clock::time_point now) const {
  const bool cache_fresh =
      config_.market_cache_minutes > 0 && !cached_markets_.empty() &&
      now - cache_fetched_at_ <
          std::chrono::minutes{config_.market_cache_minutes};
  if (!cache_fresh) {
    cached_markets_ = fetch_markets(rest_, config_.event_series);
    cache_fetched_at_ = now;
  }
  const auto &markets = cached_markets_;

  std::vector<MarketScore> candidates;
  candidates.reserve(markets.size());

  const int today_key = utc_date_key(now);
  for (const auto &market : markets) {
    if (market.status != "active") {
      continue;
    }
    if (market.yes_bid_cents == 0 || market.yes_ask_cents == 0) {
      continue;
    }
    const auto event_date_key = ticker_event_date_key(market.ticker);
    if (event_date_key.has_value() && *event_date_key < today_key) {
      continue;
    }

    const int mid = (market.yes_bid_cents + market.yes_ask_cents) / 2;
    const int spread = market.yes_ask_cents - market.yes_bid_cents;

    if (mid < config_.min_price_cents || mid > config_.max_price_cents) {
      continue;
    }
    if (spread < config_.min_spread_cents ||
        spread > config_.max_spread_cents) {
      continue;
    }
    if (market.volume_24h < config_.min_volume_24h) {
      continue;
    }

    const double seconds_to_close =
        std::chrono::duration<double>(market.close_time - now).count();
    const double days = seconds_to_close / kSecondsPerDay;
    if (days < config_.min_days_to_close || days > config_.max_days_to_close) {
      continue;
    }

    MarketScore scored;
    scored.ticker = market.ticker;
    scored.title = market.title;
    scored.category = market.category;
    scored.mid_price_cents = mid;
    scored.spread_cents = spread;
    scored.volume_24h = market.volume_24h;
    scored.days_to_close = days;
    candidates.push_back(std::move(scored));
  }

  // Find max volume among candidates for log-normalization.
  double max_volume = 1.0;
  for (const auto &candidate : candidates) {
    if (candidate.volume_24h > max_volume) {
      max_volume = candidate.volume_24h;
    }
  }

  const double max_reward =
      join_incentives(candidates, rest_.get_incentive_programs());

  const SpreadCurve curve = spread_curve(config_);
  for (auto &candidate : candidates) {
    candidate.score =
        compute_score(candidate, max_volume, curve) +
        (config_.incentive_weight * incentive_term(candidate, max_reward));
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const MarketScore &lhs, const MarketScore &rhs) {
              return lhs.score > rhs.score;
            });

  admit_finalists(candidates, top_n, now);

  if (static_cast<int>(candidates.size()) > top_n) {
    candidates.resize(static_cast<std::size_t>(top_n));
  }

  return candidates;
}

void TickerScanner::admit_finalists(
    std::vector<MarketScore> &candidates, int top_n,
    std::chrono::system_clock::time_point now) const {
  const bool check_trades = config_.max_stale_trade_minutes > 0 ||
                            config_.min_trades_per_hour > 0 ||
                            config_.min_trade_price_range_cents > 0 ||
                            config_.min_minority_flow_ratio > 0.0;
  const bool check_book = config_.min_spread_cents > 0;
  const bool check_reversion = config_.min_reversion_kappa > 0.0;
  if (!check_trades && !check_book && !check_reversion) {
    return;
  }
  std::vector<MarketScore> admitted;
  for (auto &candidate : candidates) {
    if (static_cast<int>(admitted.size()) >= top_n) {
      break;
    }
    if (check_trades && !passes_flow_admission(candidate.ticker, now)) {
      continue;
    }
    if (check_book && !passes_book_admission(candidate.ticker)) {
      continue;
    }
    if (check_reversion && !passes_reversion_admission(candidate.ticker, now)) {
      continue;
    }
    admitted.push_back(std::move(candidate));
  }
  candidates = std::move(admitted);
}

bool TickerScanner::passes_flow_admission(
    const std::string &ticker,
    std::chrono::system_clock::time_point now) const {
  const int range_probe = config_.min_trade_price_range_cents > 0
                              ? ScannerConfig::kDefaultTapeProbeTrades
                              : 1;
  const int flow_probe = config_.min_minority_flow_ratio > 0.0
                             ? ScannerConfig::kDefaultTapeProbeTrades
                             : 1;
  const int probe_limit =
      std::max({config_.min_trades_per_hour, range_probe, flow_probe, 1});
  const auto trades = rest_.get_recent_trades(ticker, probe_limit);
  if (!trades.has_value()) {
    return true;
  }
  if (trades->empty()) {
    get_logger()->info("scanner: dropped ticker={} — no public trades", ticker);
    return false;
  }
  if (config_.max_stale_trade_minutes > 0) {
    const auto staleness_cutoff =
        now - std::chrono::minutes{config_.max_stale_trade_minutes};
    if (trades->front().timestamp < staleness_cutoff) {
      get_logger()->info(
          "scanner: dropped ticker={} — last trade {}m ago (limit {}m)", ticker,
          std::chrono::duration_cast<std::chrono::minutes>(
              now - trades->front().timestamp)
              .count(),
          config_.max_stale_trade_minutes);
      return false;
    }
  }
  const auto hour_cutoff = now - std::chrono::hours{1};
  if (config_.min_trades_per_hour > 0) {
    const auto needed = static_cast<std::size_t>(config_.min_trades_per_hour);
    if (trades->size() < needed ||
        (*trades)[needed - 1].timestamp < hour_cutoff) {
      get_logger()->info(
          "scanner: dropped ticker={} — fewer than {} trades in the last hour",
          ticker, config_.min_trades_per_hour);
      return false;
    }
  }
  if (config_.min_trade_price_range_cents > 0) {
    const auto lookback_cutoff =
        now - std::chrono::minutes{config_.tape_range_lookback_minutes};
    if (!tape_shows_price_discovery(*trades, lookback_cutoff, ticker)) {
      return false;
    }
  }
  if (config_.min_minority_flow_ratio > 0.0 &&
      !tape_is_two_sided(*trades, hour_cutoff, ticker)) {
    return false;
  }
  return true;
}

bool TickerScanner::passes_reversion_admission(
    const std::string &ticker,
    std::chrono::system_clock::time_point now) const {
  const auto end_ts =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  const auto start_ts =
      end_ts - static_cast<long long>(config_.reversion_window_minutes) * 60;
  const auto candles = rest_.get_candlesticks(ticker, start_ts, end_ts);
  if (!candles.has_value()) {
    return true;
  }
  std::vector<int> closes;
  for (const auto &candle : *candles) {
    if (candle.close_cents.has_value()) {
      closes.push_back(*candle.close_cents);
    }
  }
  if (closes.size() < 2) {
    get_logger()->info(
        "scanner: dropped ticker={} — too few traded candles to judge "
        "reversion",
        ticker);
    return false;
  }
  double path_variation = 0.0;
  for (std::size_t idx = 1; idx < closes.size(); ++idx) {
    path_variation += std::abs(closes[idx] - closes[idx - 1]);
  }
  const double net_move = closes.back() - closes.front();
  const double trend_cost = net_move * net_move;
  if (path_variation < config_.min_reversion_kappa * trend_cost) {
    get_logger()->info(
        "scanner: dropped ticker={} — trending (K={:.0f} < {:.1f}*z^2={:.0f});"
        " makers harvest wiggle, trend is cost squared",
        ticker, path_variation, config_.min_reversion_kappa, trend_cost);
    return false;
  }
  return true;
}

bool TickerScanner::tape_is_two_sided(
    const std::vector<PublicTrade> &trades,
    std::chrono::system_clock::time_point hour_cutoff,
    const std::string &ticker) const {
  double yes_volume = 0.0;
  double no_volume = 0.0;
  int yes_prints = 0;
  int no_prints = 0;
  for (const auto &trade : trades) {
    if (trade.timestamp < hour_cutoff) {
      break;
    }
    if (trade.taker_side == Side::Yes) {
      yes_volume += trade.quantity.contracts();
      ++yes_prints;
    } else {
      no_volume += trade.quantity.contracts();
      ++no_prints;
    }
  }
  const bool has_volume = (yes_volume + no_volume) > 0.0;
  const double minority =
      has_volume ? std::min(yes_volume, no_volume)
                 : static_cast<double>(std::min(yes_prints, no_prints));
  const double total = has_volume ? yes_volume + no_volume
                                  : static_cast<double>(yes_prints + no_prints);
  if (total <= 0.0) {
    get_logger()->info(
        "scanner: dropped ticker={} — no prints in the last hour to judge "
        "flow balance",
        ticker);
    return false;
  }
  const double ratio = minority / total;
  if (ratio < config_.min_minority_flow_ratio) {
    get_logger()->info(
        "scanner: dropped ticker={} — one-way flow (minority side {:.0f}% < "
        "{:.0f}%); round trips unlikely",
        ticker, ratio * 100.0, config_.min_minority_flow_ratio * 100.0);
    return false;
  }
  return true;
}

bool TickerScanner::tape_shows_price_discovery(
    const std::vector<PublicTrade> &trades,
    std::chrono::system_clock::time_point lookback_cutoff,
    const std::string &ticker) const {
  int min_price = kMaxPriceCents + 1;
  int max_price = 0;
  int recent_count = 0;
  for (const auto &trade : trades) {
    if (trade.timestamp < lookback_cutoff) {
      break;
    }
    ++recent_count;
    min_price = std::min(min_price, trade.yes_price_cents);
    max_price = std::max(max_price, trade.yes_price_cents);
  }
  if (recent_count < 2) {
    get_logger()->info(
        "scanner: dropped ticker={} — too few recent prints to show price "
        "discovery",
        ticker);
    return false;
  }
  const int range = max_price - min_price;
  if (range < config_.min_trade_price_range_cents) {
    get_logger()->info(
        "scanner: dropped ticker={} — tape pinned in a {}c range over the "
        "last {}m; likely a determined market",
        ticker, range, config_.tape_range_lookback_minutes);
    return false;
  }
  return true;
}

bool TickerScanner::passes_book_admission(const std::string &ticker) const {
  try {
    LocalOrderbook book;
    book.apply_snapshot(rest_.get_orderbook(ticker));
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid.has_value() && ask.has_value() &&
        ask->price_cents - bid->price_cents < config_.min_spread_cents) {
      get_logger()->info(
          "scanner: dropped ticker={} — live book spread {}c tighter than {}c",
          ticker, ask->price_cents - bid->price_cents,
          config_.min_spread_cents);
      return false;
    }
    return true;
  } catch (const std::exception &ex) {
    get_logger()->debug("scanner: book probe failed ticker={}: {}", ticker,
                        ex.what());
    return true;
  }
}

} // namespace kalshi
