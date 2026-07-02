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
  IPricingModel() = default;
  virtual ~IPricingModel() = default;
  IPricingModel(const IPricingModel &) = delete;
  IPricingModel &operator=(const IPricingModel &) = delete;
  IPricingModel(IPricingModel &&) = delete;
  IPricingModel &operator=(IPricingModel &&) = delete;
  [[nodiscard]] virtual double estimate(const FairValueInput &input) const = 0;
};

// Mid-price + exponential time-decay toward 50 + inventory skew + optional
// external signal. The original heuristic model; shipped as the safe baseline.
class HeuristicModel : public IPricingModel {
public:
  [[nodiscard]] double estimate(const FairValueInput &input) const override;
};

// Debias an observed market probability to the true probability under the
// Bürgi/Deng/Whelan favorite-longshot model: π* = (P − β/2) / (1 − β). Pulls
// longshots down and favorites up. Result clamped to [0.01, 0.99].
[[nodiscard]] double debias_probability(double market_prob, double beta);

// "View-based" pricing: the fair value is the bot's probability *view*, not the
// raw (biased) market mid. The view is an external estimate when supplied via
// FairValueInput::external_prob, otherwise the debiased market mid. Per
// Bürgi/Deng/Whelan, market prices carry a favorite-longshot bias (β≈0.09), so
// pricing toward the debiased probability is where systematic maker edge comes
// from. Result in cents, clamped to [1, 99].
class ViewBasedModel : public IPricingModel {
public:
  static constexpr double kDefaultBeta = 0.09;
  explicit ViewBasedModel(double beta = kDefaultBeta);
  [[nodiscard]] double estimate(const FairValueInput &input) const override;

private:
  double beta_;
};

} // namespace kalshi
