#include "theo_grid.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>

namespace kalshi {

// ---- Default production grid ----------------------------------------
//
// 5 time-to-close breakpoints (hours) × 5 mid-price breakpoints (cents).
// Values are a gentle time-decay skew: as TTC shrinks, prices drift slightly
// toward 50 (mean-reversion when time is short).
//
// These are placeholder values — the real grid is calibrated offline.

namespace {

constexpr std::size_t kDefaultRows = 5;
constexpr std::size_t kDefaultCols = 5;

using TtcRow = std::array<double, kDefaultRows>;
using MidRow = std::array<double, kDefaultCols>;
using ValueRow = std::array<double, kDefaultCols>;
using ValueTable = std::array<ValueRow, kDefaultRows>;

constexpr TtcRow kDefaultTtc = {0.25, 1.0, 4.0, 12.0, 48.0};
constexpr MidRow kDefaultMid = {10.0, 25.0, 50.0, 75.0, 90.0};

// values[ttc_idx][mid_idx]
constexpr ValueTable kDefaultValues = {{
    //  mid=10  mid=25  mid=50  mid=75  mid=90
    {12.0, 27.0, 50.0, 73.0, 88.0}, // ttc=0.25h — compress toward 50
    {11.0, 26.0, 50.0, 74.0, 89.0}, // ttc=1.0h
    {10.0, 25.0, 50.0, 75.0, 90.0}, // ttc=4.0h  — near identity
    {10.0, 25.0, 50.0, 75.0, 90.0}, // ttc=12.0h
    {10.0, 25.0, 50.0, 75.0, 90.0}, // ttc=48.0h
}};

} // namespace

TheoGridConfig TheoGridConfig::default_config() {
  TheoGridConfig cfg;
  cfg.ttc_breakpoints.assign(kDefaultTtc.begin(), kDefaultTtc.end());
  cfg.mid_breakpoints.assign(kDefaultMid.begin(), kDefaultMid.end());
  cfg.values.resize(kDefaultRows);
  for (std::size_t row = 0; row < kDefaultRows; ++row) {
    cfg.values.at(row).assign(kDefaultValues.at(row).begin(),
                              kDefaultValues.at(row).end());
  }
  return cfg;
}

// ---- TheoGrid -------------------------------------------------------

TheoGrid::TheoGrid(TheoGridConfig config) : config_{std::move(config)} {
  assert(!config_.ttc_breakpoints.empty());
  assert(!config_.mid_breakpoints.empty());
  assert(config_.values.size() == config_.ttc_breakpoints.size());
}

double TheoGrid::lookup(double ttc_hours, double mid_cents) const {
  const auto &ttc_bp = config_.ttc_breakpoints;
  const auto &mid_bp = config_.mid_breakpoints;
  const auto &vals = config_.values;

  // Find bracketing index along ttc axis (clamped).
  auto ttc_it = std::lower_bound(ttc_bp.begin(), ttc_bp.end(), ttc_hours);
  std::size_t ttc_hi =
      static_cast<std::size_t>(ttc_it - ttc_bp.begin()); // first index >= ttc
  if (ttc_hi == 0) {
    ttc_hi = 1; // clamp below
  } else if (ttc_hi >= ttc_bp.size()) {
    ttc_hi = ttc_bp.size() - 1; // clamp above
  }
  const std::size_t ttc_lo = ttc_hi - 1;

  // Find bracketing index along mid axis (clamped).
  auto mid_it = std::lower_bound(mid_bp.begin(), mid_bp.end(), mid_cents);
  std::size_t mid_hi =
      static_cast<std::size_t>(mid_it - mid_bp.begin()); // first index >= mid
  if (mid_hi == 0) {
    mid_hi = 1;
  } else if (mid_hi >= mid_bp.size()) {
    mid_hi = mid_bp.size() - 1;
  }
  const std::size_t mid_lo = mid_hi - 1;

  // Interpolation fractions.
  const double ttc_range = ttc_bp.at(ttc_hi) - ttc_bp.at(ttc_lo);
  const double ttc_frac =
      (ttc_range > 0.0) ? (ttc_hours - ttc_bp.at(ttc_lo)) / ttc_range : 0.0;
  const double mid_range = mid_bp.at(mid_hi) - mid_bp.at(mid_lo);
  const double mid_frac =
      (mid_range > 0.0) ? (mid_cents - mid_bp.at(mid_lo)) / mid_range : 0.0;

  // Clamp fractions to [0, 1] to handle boundary edge cases.
  const double ttc_alpha = std::clamp(ttc_frac, 0.0, 1.0);
  const double mid_alpha = std::clamp(mid_frac, 0.0, 1.0);

  // Bilinear interpolation: lerp over mid at each ttc row, then lerp over ttc.
  const double val_lo =
      vals.at(ttc_lo).at(mid_lo) +
      mid_alpha * (vals.at(ttc_lo).at(mid_hi) - vals.at(ttc_lo).at(mid_lo));
  const double val_hi =
      vals.at(ttc_hi).at(mid_lo) +
      mid_alpha * (vals.at(ttc_hi).at(mid_hi) - vals.at(ttc_hi).at(mid_lo));

  return val_lo + ttc_alpha * (val_hi - val_lo);
}

} // namespace kalshi
