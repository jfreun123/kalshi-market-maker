#include "adverse_selection.hpp"

#include <algorithm>
#include <chrono>

namespace kalshi {

AdverseSelectionGuard::AdverseSelectionGuard(AdverseSelectionConfig config)
    : config_{config} {}

bool AdverseSelectionGuard::record_fill(
    std::string_view ticker, std::chrono::system_clock::time_point timestamp) {
  const std::string ticker_str{ticker};
  auto &times = fill_times_[ticker_str];

  // Purge fills older than window_seconds relative to the new fill.
  const auto window =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::duration<double>{config_.window_seconds});
  const auto cutoff = timestamp - window;
  times.erase(std::remove_if(times.begin(), times.end(),
                             [cutoff](const auto &tp) { return tp < cutoff; }),
              times.end());

  times.push_back(timestamp);
  return static_cast<int>(times.size()) >= config_.fill_threshold;
}

void AdverseSelectionGuard::reset(std::string_view ticker) {
  fill_times_.erase(std::string{ticker});
}

} // namespace kalshi
