#pragma once
// Rolling mid-price drift estimator (PLAN item 60a). Keeps a per-market
// window of (time, mid) samples from the quote stream and fits an OLS slope
// with its t-statistic — the significance gate that separates a real trend
// (makers bleed quoting into it) from harvestable two-sided chop. Consumers
// scale a fair-value lean by the slope only when |t| clears their threshold;
// the Quoter additionally weights the lean by tape-volume confirmation
// (Blume-Easley-O'Hara: drift on volume is precise information, drift on a
// thin tape is only partially trustworthy).

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace kalshi {

struct DriftEstimatorConfig {
  static constexpr int kDefaultWindowSeconds = 60;
  static constexpr int kDefaultMinSamples = 12;
  int window_seconds{kDefaultWindowSeconds};
  int min_samples{kDefaultMinSamples};
};

struct DriftSignal {
  double slope_cents_per_minute{0.0};
  double t_stat{0.0};
  int samples{0};
};

class DriftEstimator {
public:
  using TimePoint = std::chrono::system_clock::time_point;

  explicit DriftEstimator(DriftEstimatorConfig config);

  void add_sample(std::string_view ticker, TimePoint at, double mid_cents);
  [[nodiscard]] std::optional<DriftSignal> signal(std::string_view ticker,
                                                  TimePoint now) const;
  void forget(std::string_view ticker);

private:
  DriftEstimatorConfig config_;
  std::unordered_map<std::string, std::deque<std::pair<TimePoint, double>>>
      samples_;
};

} // namespace kalshi
