#include "fair_value.hpp"

#include <algorithm>
#include <cmath>

namespace kalshi {

// Exponential time-decay constant: prices fade to 50 with this time horizon.
constexpr double kDecayTimeConstantHours = 24.0;
// Inventory skew: fair value shifts this many cents per net YES contract held.
constexpr double kInventorySkewCentsPerContract = 0.05;
// Weight given to the external signal when blending with the model estimate.
constexpr double kExternalSignalWeight = 0.3;
// Valid price range for a Kalshi YES contract.
constexpr double kMinValidCents = 1.0;
constexpr double kMaxValidCents = 99.0;
constexpr double kMidPointCents = 50.0;
constexpr double kContractMaxCents = 100.0;

double FairValueEngine::estimate(const FairValueInput &input) {
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

} // namespace kalshi
