#include "engine/flow_imbalance.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace kalshi {

// Floor for the ratio denominator: when the lighter side saw zero flow, divide
// by one contract instead so the ratio stays finite rather than dividing by 0.
constexpr double kMinRatioDenominator{1.0};

FlowImbalanceGuard::FlowImbalanceGuard(FlowImbalanceConfig config)
    : config_{config} {}

void FlowImbalanceGuard::record_fill(std::string_view ticker, Side side,
                                     Quantity quantity, TimePoint when) {
  auto &entries = fills_[std::string{ticker}];
  entries.push_back({when, side, quantity});

  // Evict entries that have fallen out of the window to bound memory. Fills
  // arrive roughly in time order, so dropping from the front is sufficient.
  const TimePoint cutoff = when - std::chrono::seconds{config_.window_seconds};
  while (!entries.empty() && entries.front().when < cutoff) {
    entries.pop_front();
  }
}

std::pair<Quantity, Quantity>
FlowImbalanceGuard::windowed_volume(std::string_view ticker,
                                    TimePoint now) const {
  Quantity yes_volume{};
  Quantity no_volume{};

  auto iter = fills_.find(std::string{ticker});
  if (iter == fills_.end()) {
    return {yes_volume, no_volume};
  }

  const TimePoint cutoff = now - std::chrono::seconds{config_.window_seconds};
  for (const Entry &entry : iter->second) {
    if (entry.when < cutoff) {
      continue;
    }
    if (entry.side == Side::Yes) {
      yes_volume += entry.quantity;
    } else {
      no_volume += entry.quantity;
    }
  }
  return {yes_volume, no_volume};
}

double FlowImbalanceGuard::imbalance_ratio(std::string_view ticker,
                                           TimePoint now) const {
  const auto [yes_volume, no_volume] = windowed_volume(ticker, now);
  const double high = std::max(yes_volume.contracts(), no_volume.contracts());
  const double low = std::min(yes_volume.contracts(), no_volume.contracts());
  if (high == 0.0) {
    return 1.0; // no flow → balanced
  }
  return high / std::max(low, kMinRatioDenominator);
}

std::optional<Side>
FlowImbalanceGuard::dominant_taker_side(std::string_view ticker,
                                        TimePoint now) const {
  if (!is_imbalanced(ticker, now)) {
    return std::nullopt;
  }
  const auto [yes_volume, no_volume] = windowed_volume(ticker, now);
  // We recorded OUR side; the takers bought the opposite.
  return (yes_volume > no_volume) ? Side::No : Side::Yes;
}

bool FlowImbalanceGuard::is_imbalanced(std::string_view ticker,
                                       TimePoint now) const {
  const auto [yes_volume, no_volume] = windowed_volume(ticker, now);
  if (yes_volume + no_volume <
      Quantity::from_contracts(config_.min_flow_volume)) {
    return false;
  }
  const double high = std::max(yes_volume.contracts(), no_volume.contracts());
  const double low = std::min(yes_volume.contracts(), no_volume.contracts());
  const double ratio = high / std::max(low, kMinRatioDenominator);
  return ratio > config_.imbalance_ratio_threshold;
}

void FlowImbalanceGuard::reset(std::string_view ticker) {
  fills_.erase(std::string{ticker});
}

} // namespace kalshi
