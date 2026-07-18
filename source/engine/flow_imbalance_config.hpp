#pragma once
// Rolling-window one-sided-flow detector parameters. Extracted so config
// loading does not drag in the guard.

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

} // namespace kalshi
