#pragma once

#include <optional>

namespace kalshi {

struct FairValueInput {
  double mid_cents;           // orderbook mid-price in cents [1, 99]
  double time_to_close_hours; // hours until market resolves (must be >= 0)
  int net_position;           // net YES contracts held (negative = net NO)
  std::optional<double>
      external_prob; // external probability estimate in [0, 1]
};

// Estimates the fair value of a YES contract in cents [1, 99].
//
// Model layers applied in order:
//   v1 (baseline):  start from orderbook mid-price
//   v2 (time-decay): fade extreme prices toward 50 as close approaches
//   v3 (inventory):  shade down when long YES, up when short YES
//   v4 (external):   blend an external probability signal if provided
class FairValueEngine {
public:
  [[nodiscard]] static double estimate(const FairValueInput &input);
};

} // namespace kalshi
