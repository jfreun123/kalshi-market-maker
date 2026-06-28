#include "ticker_scanner.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace kalshi {

namespace {

constexpr double kSecondsPerDay = 86400.0;

constexpr double kWeightVolume = 0.70;
constexpr double kWeightSpread = 0.30;

// Spread scoring peaks at the midpoint of the [min_spread, max_spread] filter
// range ([3, 10]c) and falls to zero at the edges.
constexpr double kSpreadMidCents = 6.5;
constexpr double kSpreadHalfRange = 3.5;

double compute_score(const MarketScore &market, double max_volume) {
  double vol_term = 0.0;
  if (max_volume > 1.0 && market.volume_24h > 1.0) {
    vol_term = std::log(market.volume_24h) / std::log(max_volume);
  }

  const double spread_term =
      1.0 -
      std::abs(static_cast<double>(market.spread_cents) - kSpreadMidCents) /
          kSpreadHalfRange;

  return kWeightVolume * vol_term + kWeightSpread * std::max(0.0, spread_term);
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

  for (auto &candidate : candidates) {
    candidate.score = compute_score(candidate, max_volume);
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const MarketScore &lhs, const MarketScore &rhs) {
              return lhs.score > rhs.score;
            });

  if (static_cast<int>(candidates.size()) > top_n) {
    candidates.resize(static_cast<std::size_t>(top_n));
  }

  return candidates;
}

} // namespace kalshi
