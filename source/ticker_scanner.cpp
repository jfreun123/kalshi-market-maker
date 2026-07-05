#include "ticker_scanner.hpp"

#include "logger.hpp"
#include "orderbook.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace kalshi {

namespace {

constexpr double kSecondsPerDay = 86400.0;
constexpr double kCentiCentsPerDollar = 10000.0;

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
  const auto markets = fetch_markets(rest_, config_.event_series);

  std::vector<MarketScore> candidates;
  candidates.reserve(markets.size());

  for (const auto &market : markets) {
    if (market.status != "active") {
      continue;
    }
    if (market.yes_bid_cents == 0 || market.yes_ask_cents == 0) {
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
  const bool check_trades =
      config_.max_stale_trade_minutes > 0 || config_.min_trades_per_hour > 0;
  const bool check_book = config_.min_spread_cents > 0;
  if (!check_trades && !check_book) {
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
    admitted.push_back(std::move(candidate));
  }
  candidates = std::move(admitted);
}

bool TickerScanner::passes_flow_admission(
    const std::string &ticker,
    std::chrono::system_clock::time_point now) const {
  const int probe_limit = std::max(config_.min_trades_per_hour, 1);
  const auto trade_times = rest_.get_recent_trade_times(ticker, probe_limit);
  if (!trade_times.has_value()) {
    return true;
  }
  if (trade_times->empty()) {
    get_logger()->info("scanner: dropped ticker={} — no public trades", ticker);
    return false;
  }
  if (config_.max_stale_trade_minutes > 0) {
    const auto staleness_cutoff =
        now - std::chrono::minutes{config_.max_stale_trade_minutes};
    if (trade_times->front() < staleness_cutoff) {
      get_logger()->info(
          "scanner: dropped ticker={} — last trade {}m ago (limit {}m)",
          ticker,
          std::chrono::duration_cast<std::chrono::minutes>(
              now - trade_times->front())
              .count(),
          config_.max_stale_trade_minutes);
      return false;
    }
  }
  if (config_.min_trades_per_hour > 0) {
    const auto hour_cutoff = now - std::chrono::hours{1};
    const auto needed = static_cast<std::size_t>(config_.min_trades_per_hour);
    if (trade_times->size() < needed ||
        (*trade_times)[needed - 1] < hour_cutoff) {
      get_logger()->info(
          "scanner: dropped ticker={} — fewer than {} trades in the last hour",
          ticker, config_.min_trades_per_hour);
      return false;
    }
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
