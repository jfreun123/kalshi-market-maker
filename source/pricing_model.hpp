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

// Strategy interface: all pricing models implement this.
class IPricingModel {
public:
  virtual ~IPricingModel() = default;
  [[nodiscard]] virtual double estimate(const FairValueInput &input) const = 0;
};

// Mid-price + exponential time-decay toward 50 + inventory skew + optional
// external signal. The original heuristic model; shipped as the safe baseline.
class HeuristicModel : public IPricingModel {
public:
  [[nodiscard]] double estimate(const FairValueInput &input) const override;
};

} // namespace kalshi
