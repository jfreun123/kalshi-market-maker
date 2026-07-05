#include "ticker_scanner.hpp"

#include "logger.hpp"

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

  if (config_.max_stale_trade_minutes > 0) {
    const auto staleness_cutoff =
        now - std::chrono::minutes{config_.max_stale_trade_minutes};
    std::vector<MarketScore> fresh;
    for (auto &candidate : candidates) {
      if (static_cast<int>(fresh.size()) >= top_n) {
        break;
      }
      const auto last_trade = rest_.get_last_trade_time(candidate.ticker);
      if (last_trade.has_value() && *last_trade < staleness_cutoff) {
        get_logger()->info(
            "scanner: dropped ticker={} — last trade {}m ago (limit {}m)",
            candidate.ticker,
            std::chrono::duration_cast<std::chrono::minutes>(now - *last_trade)
                .count(),
            config_.max_stale_trade_minutes);
        continue;
      }
      fresh.push_back(std::move(candidate));
    }
    candidates = std::move(fresh);
  }

  if (static_cast<int>(candidates.size()) > top_n) {
    candidates.resize(static_cast<std::size_t>(top_n));
  }

  return candidates;
}

} // namespace kalshi
