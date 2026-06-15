#include "fair_value.hpp"

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

kalshi::FairValueInput make_input(double mid_cents,
                                  double time_to_close_hours = kLargeTimeHours,
                                  int net_position = 0,
                                  std::optional<double> external_prob = {}) {
  return kalshi::FairValueInput{mid_cents, time_to_close_hours, net_position,
                                external_prob};
}

} // namespace

// ---- Tests ----

TEST(FairValueTest, EstimateReturnsMidPriceForBaselineInputs) {
  // Large time → no decay; zero position → no skew; no external.
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kTestPriceCents, kLargeTimeHours));
  EXPECT_NEAR(result, kTestPriceCents, kTolerance);
}

TEST(FairValueTest, OutputClampedToMinimumAtBelowMinMidPrice) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kBelowMinPriceCents, kLargeTimeHours));
  EXPECT_NEAR(result, kMinValidCents, kTolerance);
}

TEST(FairValueTest, OutputClampedToMaximumAtAboveMaxMidPrice) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kAboveMaxPriceCents, kLargeTimeHours));
  EXPECT_NEAR(result, kMaxValidCents, kTolerance);
}

TEST(FairValueTest, TimeDecayPullsHighPriceTowardFifty) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kHighPriceCents, kSmallTimeHours));
  EXPECT_LT(result, kHighPriceCents);
}

TEST(FairValueTest, TimeDecayPullsLowPriceTowardFifty) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kLowPriceCents, kSmallTimeHours));
  EXPECT_GT(result, kLowPriceCents);
}

TEST(FairValueTest, TimeDecayNegligibleWhenFarFromClose) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kHighPriceCents, kLargeTimeHours));
  EXPECT_NEAR(result, kHighPriceCents, kTolerance);
}

TEST(FairValueTest, InventorySkewReducesFairValueWhenLong) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kMidPriceCents, kLargeTimeHours, kLongPositionContracts));
  EXPECT_LT(result, kMidPriceCents);
}

TEST(FairValueTest, InventorySkewIncreasesFairValueWhenShort) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kMidPriceCents, kLargeTimeHours, kShortPositionContracts));
  EXPECT_GT(result, kMidPriceCents);
}

TEST(FairValueTest, ZeroPositionDoesNotSkewFairValue) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kTestPriceCents, kLargeTimeHours, 0));
  EXPECT_NEAR(result, kTestPriceCents, kTolerance);
}

TEST(FairValueTest, ExternalProbabilityBlendedIntoEstimate) {
  // mid=50, external=80%: blended result should be between 50 and 80.
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kMidPriceCents, kLargeTimeHours, 0, kExternalProbHigh));
  EXPECT_GT(result, kMidPriceCents);
  EXPECT_LT(result, kExternalProbHighCents);
}

TEST(FairValueTest, AbsentExternalProbabilityGivesBaselineResult) {
  const double result = kalshi::FairValueEngine::estimate(
      make_input(kTestPriceCents, kLargeTimeHours));
  EXPECT_NEAR(result, kTestPriceCents, kTolerance);
}

TEST(FairValueTest, FairValueMonotonicallyDecreasesWithIncreasingLongPosition) {
  const double result_small = kalshi::FairValueEngine::estimate(
      make_input(kMidPriceCents, kLargeTimeHours, kSmallLongPosition));
  const double result_larger = kalshi::FairValueEngine::estimate(
      make_input(kMidPriceCents, kLargeTimeHours, kLargerLongPosition));
  EXPECT_GT(result_small, result_larger);
}
