#pragma once

#include <vector>

namespace kalshi {

struct TheoGridConfig {
  // Breakpoints along the time-to-close axis (hours), ascending.
  std::vector<double> ttc_breakpoints;
  // Breakpoints along the mid-price axis (cents), ascending.
  std::vector<double> mid_breakpoints;
  // 2-D table: values[ttc_idx][mid_idx] → fair-value cents.
  // Dimensions must match ttc_breakpoints.size() × mid_breakpoints.size().
  std::vector<std::vector<double>> values;

  // Factory for a sensible production default.
  [[nodiscard]] static TheoGridConfig default_config();
};

// Bilinear interpolation lookup table for fast fair-value estimation.
//
// Inputs outside the breakpoint range are clamped to the boundary row/column.
class TheoGrid {
public:
  explicit TheoGrid(TheoGridConfig config);

  // Returns the interpolated fair-value (cents) for the given inputs.
  [[nodiscard]] double lookup(double ttc_hours, double mid_cents) const;

private:
  TheoGridConfig config_;
};

} // namespace kalshi
