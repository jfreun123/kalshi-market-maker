#include "fair_value.hpp"
#include "pricing_model.hpp"

#include <gtest/gtest.h>

// ---- Test constants ----

namespace {

constexpr double kLargeTimeHours = 1'000.0; // far from close → negligible decay
constexpr double kSmallTimeHours = 0.01;    // very close → strong decay
constexpr double kMidPriceCents = 50.0;
constexpr double kHighPriceCents = 90.0;
constexpr double kLowPriceCents = 10.0;
constexpr double kTestPriceCents = 52.0;
constexpr double kBelowMinPriceCents = 0.0;
constexpr double kAboveMaxPriceCents = 100.0;
constexpr double kMinValidCents = 1.0;
constexpr double kMaxValidCents = 99.0;
constexpr int kLongPositionContracts = 50;
constexpr int kShortPositionContracts = -50;
constexpr int kSmallLongPosition = 10;
constexpr int kLargerLongPosition = 20;
constexpr double kExternalProbHigh = 0.80;      // 80% → 80 cents
constexpr double kExternalProbHighCents = 80.0; // upper bound for blend test
constexpr double kTolerance = 0.01;

// Stub model that always returns a fixed value — used to test FairValueEngine
// delegation without depending on HeuristicModel behaviour.
class FixedModel : public kalshi::IPricingModel {
public:
  explicit FixedModel(double value) : value_{value} {}
  [[nodiscard]] double
  estimate(const kalshi::FairValueInput & /*input*/) const override {
    return value_;
  }

private:
  double value_;
};

kalshi::FairValueInput make_input(double mid_cents,
                                  double time_to_close_hours = kLargeTimeHours,
                                  int net_position = 0,
                                  std::optional<double> external_prob = {}) {
  return kalshi::FairValueInput{mid_cents, time_to_close_hours, net_position,
                                external_prob};
}

} // namespace

// ---- HeuristicModel tests ----

TEST(HeuristicModelTest, ReturnsMidPriceForBaselineInputs) {
  kalshi::HeuristicModel model;
  const double result = model.estimate(make_input(kTestPriceCents));
  EXPECT_NEAR(result, kTestPriceCents, kTolerance);
}

TEST(HeuristicModelTest, OutputClampedToMinimumBelowMinMidPrice) {
  kalshi::HeuristicModel model;
  const double result =
      model.estimate(make_input(kBelowMinPriceCents, kLargeTimeHours));
  EXPECT_NEAR(result, kMinValidCents, kTolerance);
}

TEST(HeuristicModelTest, OutputClampedToMaximumAboveMaxMidPrice) {
  kalshi::HeuristicModel model;
  const double result =
      model.estimate(make_input(kAboveMaxPriceCents, kLargeTimeHours));
  EXPECT_NEAR(result, kMaxValidCents, kTolerance);
}

TEST(HeuristicModelTest, TimeDecayPullsHighPriceTowardFifty) {
  kalshi::HeuristicModel model;
  const double result =
      model.estimate(make_input(kHighPriceCents, kSmallTimeHours));
  EXPECT_LT(result, kHighPriceCents);
}

TEST(HeuristicModelTest, TimeDecayPullsLowPriceTowardFifty) {
  kalshi::HeuristicModel model;
  const double result =
      model.estimate(make_input(kLowPriceCents, kSmallTimeHours));
  EXPECT_GT(result, kLowPriceCents);
}

TEST(HeuristicModelTest, TimeDecayNegligibleWhenFarFromClose) {
  kalshi::HeuristicModel model;
  const double result =
      model.estimate(make_input(kHighPriceCents, kLargeTimeHours));
  EXPECT_NEAR(result, kHighPriceCents, kTolerance);
}

TEST(HeuristicModelTest, InventorySkewReducesFairValueWhenLong) {
  kalshi::HeuristicModel model;
  const double result = model.estimate(
      make_input(kMidPriceCents, kLargeTimeHours, kLongPositionContracts));
  EXPECT_LT(result, kMidPriceCents);
}

TEST(HeuristicModelTest, InventorySkewIncreasesFairValueWhenShort) {
  kalshi::HeuristicModel model;
  const double result = model.estimate(
      make_input(kMidPriceCents, kLargeTimeHours, kShortPositionContracts));
  EXPECT_GT(result, kMidPriceCents);
}

TEST(HeuristicModelTest, ZeroPositionDoesNotSkewFairValue) {
  kalshi::HeuristicModel model;
  const double result = model.estimate(make_input(kTestPriceCents));
  EXPECT_NEAR(result, kTestPriceCents, kTolerance);
}

TEST(HeuristicModelTest, ExternalProbabilityBlendedIntoEstimate) {
  kalshi::HeuristicModel model;
  const double result = model.estimate(
      make_input(kMidPriceCents, kLargeTimeHours, 0, kExternalProbHigh));
  EXPECT_GT(result, kMidPriceCents);
  EXPECT_LT(result, kExternalProbHighCents);
}

TEST(HeuristicModelTest, AbsentExternalProbabilityGivesBaselineResult) {
  kalshi::HeuristicModel model;
  const double result = model.estimate(make_input(kTestPriceCents));
  EXPECT_NEAR(result, kTestPriceCents, kTolerance);
}

TEST(HeuristicModelTest,
     FairValueMonotonicallyDecreasesWithIncreasingLongPosition) {
  kalshi::HeuristicModel model;
  const double result_small = model.estimate(
      make_input(kMidPriceCents, kLargeTimeHours, kSmallLongPosition));
  const double result_larger = model.estimate(
      make_input(kMidPriceCents, kLargeTimeHours, kLargerLongPosition));
  EXPECT_GT(result_small, result_larger);
}

// ---- FairValueEngine delegation tests ----

TEST(FairValueEngineTest, DelegatesEstimateToInjectedModel) {
  constexpr double kFixedValue = 63.0;
  kalshi::FairValueEngine engine{std::make_unique<FixedModel>(kFixedValue)};
  const double result = engine.estimate(make_input(kTestPriceCents));
  EXPECT_DOUBLE_EQ(result, kFixedValue);
}

TEST(FairValueEngineTest, PassesInputUnchangedToModel) {
  // Verify the input fields reach the model by using a model that returns
  // mid_cents so we can assert the value was forwarded correctly.
  class EchoMidModel : public kalshi::IPricingModel {
  public:
    [[nodiscard]] double
    estimate(const kalshi::FairValueInput &input) const override {
      return input.mid_cents;
    }
  };
  constexpr double kMid = 37.0;
  kalshi::FairValueEngine engine{std::make_unique<EchoMidModel>()};
  const double result = engine.estimate(make_input(kMid));
  EXPECT_DOUBLE_EQ(result, kMid);
}
