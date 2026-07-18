#include "engine/pricing_model.hpp"

#include <algorithm>
#include <cmath>

namespace kalshi {

constexpr double kDecayTimeConstantHours = 24.0;
constexpr double kInventorySkewCentsPerContract = 0.05;
constexpr double kExternalSignalWeight = 0.3;
constexpr double kMinValidCents = 1.0;
constexpr double kMaxValidCents = 99.0;
constexpr double kMidPointCents = 50.0;
constexpr double kContractMaxCents = 100.0;

double HeuristicModel::estimate(const FairValueInput &input) const {
  // v1: baseline — start from the orderbook mid.
  double value = input.mid_cents;

  // v2: time-decay — pull extreme prices toward 50 as expiry approaches.
  const double decay =
      std::exp(-input.time_to_close_hours / kDecayTimeConstantHours);
  value += (kMidPointCents - value) * decay;

  // v3: inventory skew — being long YES makes it cheaper to sell YES.
  value -=
      static_cast<double>(input.net_position) * kInventorySkewCentsPerContract;

  // v4: external signal — blend in an external probability if one is provided.
  if (input.external_prob.has_value()) {
    const double ext_cents = *input.external_prob * kContractMaxCents;
    value = (1.0 - kExternalSignalWeight) * value +
            kExternalSignalWeight * ext_cents;
  }

  return std::clamp(value, kMinValidCents, kMaxValidCents);
}

ClearingPriceModel::ClearingPriceModel(double tape_weight)
    : tape_weight_{tape_weight} {}

double ClearingPriceModel::estimate(const FairValueInput &input) const {
  double value = input.mid_cents;
  if (input.tape_vwap_cents.has_value()) {
    value = (tape_weight_ * *input.tape_vwap_cents) +
            ((1.0 - tape_weight_) * input.mid_cents);
  }
  return std::clamp(value, kMinValidCents, kMaxValidCents);
}

constexpr double kMinProb = 0.01;
constexpr double kMaxProb = 0.99;
constexpr double kBetaMax = 0.95; // keep (1 - beta) safely positive

double debias_probability(double market_prob, double beta) {
  const double debiased = (market_prob - beta / 2.0) / (1.0 - beta);
  return std::clamp(debiased, kMinProb, kMaxProb);
}

ViewBasedModel::ViewBasedModel(double beta)
    : beta_{std::clamp(beta, 0.0, kBetaMax)} {}

double ViewBasedModel::estimate(const FairValueInput &input) const {
  // An external view, when supplied, overrides the market-derived one.
  const double view_prob =
      input.external_prob.has_value()
          ? std::clamp(*input.external_prob, kMinProb, kMaxProb)
          : debias_probability(input.mid_cents / kContractMaxCents, beta_);
  return view_prob * kContractMaxCents;
}

} // namespace kalshi
