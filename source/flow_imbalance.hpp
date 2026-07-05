#pragma once

#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace kalshi {

struct FlowImbalanceConfig {
  static constexpr int kDefaultWindowSeconds = 300; // 5-minute rolling window
  static constexpr double kDefaultRatioThreshold =
      2.0;                                         // one side ≥ 2× the other
  static constexpr int kDefaultMinFlowVolume = 20; // ignore thin/noisy samples

  int window_seconds = kDefaultWindowSeconds;
  double imbalance_ratio_threshold = kDefaultRatioThreshold;
  int min_flow_volume = kDefaultMinFlowVolume;
};

// Tracks the bot's recent fill volume per side, per ticker, over a rolling time
// window. Heavy one-sided flow — the bot accumulating mostly YES or mostly NO —
// is the strongest predictor of adverse terminal directional exposure (Palumbo
// 2026: side-weighted volume imbalance dominates LP returns, not fill rate).
// The Quoter widens its spread while a market is imbalanced to demand more
// compensation for the adverse flow.
//
// Read-only queries take `now` so they are testable and side-effect-free;
// `record_fill` evicts entries older than the window to bound memory.
class FlowImbalanceGuard {
public:
  using Clock = std::chrono::system_clock;
  using TimePoint = Clock::time_point;

  explicit FlowImbalanceGuard(FlowImbalanceConfig config);

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void record_fill(std::string_view ticker, Side side, Quantity quantity,
                   TimePoint when = Clock::now());

  // Larger-side / smaller-side fill volume over the window; 1.0 = balanced (or
  // no flow). Grows without bound as flow concentrates on one side.
  [[nodiscard]] double imbalance_ratio(std::string_view ticker,
                                       TimePoint now = Clock::now()) const;

  // True when the window holds at least min_flow_volume contracts and the ratio
  // exceeds imbalance_ratio_threshold.
  // When imbalanced, the side the TAKERS are buying (opposite of our fills:
  // we filled NO means takers bought YES). Empty when balanced. Feeds the
  // quoter's directional lean (item 32).
  [[nodiscard]] std::optional<Side>
  dominant_taker_side(std::string_view ticker,
                      TimePoint now = Clock::now()) const;

  [[nodiscard]] bool is_imbalanced(std::string_view ticker,
                                   TimePoint now = Clock::now()) const;

  void reset(std::string_view ticker);

private:
  struct Entry {
    TimePoint when;
    Side side;
    Quantity quantity;
  };

  // {yes_volume, no_volume} for fills within [now - window, now].
  [[nodiscard]] std::pair<Quantity, Quantity>
  windowed_volume(std::string_view ticker, TimePoint now) const;

  FlowImbalanceConfig config_;
  std::unordered_map<std::string, std::deque<Entry>> fills_;
};

} // namespace kalshi
