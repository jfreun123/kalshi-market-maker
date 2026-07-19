#include "engine/drift_estimator.hpp"

#include <cmath>

namespace kalshi {

namespace {
constexpr double kSecondsPerMinute = 60.0;
constexpr double kDegenerateResidualFloor = 1e-12;
constexpr double kPerfectFitTStat = 1e9;
} // namespace

DriftEstimator::DriftEstimator(DriftEstimatorConfig config) : config_{config} {}

void DriftEstimator::add_sample(std::string_view ticker, TimePoint at,
                                double mid_cents) {
  auto &window = samples_[std::string{ticker}];
  window.emplace_back(at, mid_cents);
  const auto horizon = at - std::chrono::seconds{config_.window_seconds};
  while (!window.empty() && window.front().first < horizon) {
    window.pop_front();
  }
}

std::optional<DriftSignal> DriftEstimator::signal(std::string_view ticker,
                                                  TimePoint now) const {
  const auto found = samples_.find(std::string{ticker});
  if (found == samples_.end()) {
    return std::nullopt;
  }
  const auto horizon = now - std::chrono::seconds{config_.window_seconds};
  double sum_x = 0.0;
  double sum_y = 0.0;
  int count = 0;
  for (const auto &[at, mid] : found->second) {
    if (at < horizon) {
      continue;
    }
    sum_x += std::chrono::duration<double>(at - horizon).count();
    sum_y += mid;
    ++count;
  }
  if (count < config_.min_samples) {
    return std::nullopt;
  }
  const double mean_x = sum_x / count;
  const double mean_y = sum_y / count;
  double sxx = 0.0;
  double sxy = 0.0;
  for (const auto &[at, mid] : found->second) {
    if (at < horizon) {
      continue;
    }
    const double time_x =
        std::chrono::duration<double>(at - horizon).count() - mean_x;
    sxx += time_x * time_x;
    sxy += time_x * (mid - mean_y);
  }
  if (sxx <= 0.0) {
    return std::nullopt;
  }
  const double slope_per_second = sxy / sxx;
  double residual_sum = 0.0;
  for (const auto &[at, mid] : found->second) {
    if (at < horizon) {
      continue;
    }
    const double time_x =
        std::chrono::duration<double>(at - horizon).count() - mean_x;
    const double residual = (mid - mean_y) - slope_per_second * time_x;
    residual_sum += residual * residual;
  }
  DriftSignal result;
  result.slope_cents_per_minute = slope_per_second * kSecondsPerMinute;
  result.samples = count;
  const int degrees = count - 2;
  if (degrees <= 0 || residual_sum < kDegenerateResidualFloor) {
    result.t_stat = (slope_per_second == 0.0)
                        ? 0.0
                        : std::copysign(kPerfectFitTStat, slope_per_second);
    return result;
  }
  const double standard_error = std::sqrt(residual_sum / degrees / sxx);
  result.t_stat = slope_per_second / standard_error;
  return result;
}

void DriftEstimator::forget(std::string_view ticker) {
  samples_.erase(std::string{ticker});
}

} // namespace kalshi
