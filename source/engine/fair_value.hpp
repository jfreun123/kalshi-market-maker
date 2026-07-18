#pragma once

#include "engine/pricing_model.hpp"

#include <memory>

namespace kalshi {

// Delegates fair-value estimation to an injected IPricingModel.
// Constructed with a concrete model; estimate() simply forwards to it.
//
// Quoter creates FairValueEngine{std::make_unique<HeuristicModel>()} by
// default. Swap in a different model (calibrated, ML, RL) without touching
// Quoter.
class FairValueEngine {
public:
  explicit FairValueEngine(std::unique_ptr<IPricingModel> model);
  [[nodiscard]] double estimate(const FairValueInput &input) const;

private:
  std::unique_ptr<IPricingModel> model_;
};

} // namespace kalshi
