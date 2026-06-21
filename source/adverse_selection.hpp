#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kalshi {

struct AdverseSelectionConfig {
  static constexpr int kDefaultFillThreshold = 5;
  static constexpr double kDefaultWindowSeconds = 30.0;
  static constexpr double kDefaultCooldownSeconds = 10.0;

  int fill_threshold = kDefaultFillThreshold;
  double window_seconds = kDefaultWindowSeconds;
  double cooldown_seconds = kDefaultCooldownSeconds;
};

// Rolling-window fill-rate guard for adverse selection detection.
//
// Call record_fill() on every fill event. Returns true the first time the
// fill count within window_seconds reaches fill_threshold — signaling that the
// system is likely being picked off by an informed trader. The main loop
// should then set Constraint::kHighFillRate on RiskManager and cancel quotes.
// Call reset() after the cooldown period to re-arm the guard.
class AdverseSelectionGuard {
public:
  explicit AdverseSelectionGuard(
      AdverseSelectionConfig config = AdverseSelectionConfig{});

  // Records a fill and returns true if the threshold is breached.
  bool record_fill(std::string_view ticker,
                   std::chrono::system_clock::time_point timestamp);

  // Clears fill history for a ticker. Call after cooldown before re-entry.
  void reset(std::string_view ticker);

private:
  AdverseSelectionConfig config_;
  std::unordered_map<std::string,
                     std::vector<std::chrono::system_clock::time_point>>
      fill_times_;
};

} // namespace kalshi
