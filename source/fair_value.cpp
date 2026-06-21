#include "fair_value.hpp"

namespace kalshi {

FairValueEngine::FairValueEngine(std::unique_ptr<IPricingModel> model)
    : model_{std::move(model)} {}

double FairValueEngine::estimate(const FairValueInput &input) const {
  return model_->estimate(input);
}

} // namespace kalshi
