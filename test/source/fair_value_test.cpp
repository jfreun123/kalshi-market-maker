#include "engine/fair_value.hpp"
#include "engine/pricing_model.hpp"

#include <gtest/gtest.h>

// Integration tests: FairValueEngine wired to HeuristicModel.
// Unit tests for HeuristicModel behaviour live in pricing_model_test.cpp.

namespace {

constexpr double kLargeTimeHours = 1'000.0;
constexpr double kTestPriceCents = 52.0;
constexpr double kHighPriceCents = 90.0;
constexpr double kLowPriceCents = 10.0;
constexpr double kSmallTimeHours = 0.01;
constexpr double kTolerance = 0.01;

kalshi::FairValueEngine make_engine() {
  return kalshi::FairValueEngine{std::make_unique<kalshi::HeuristicModel>()};
}

kalshi::FairValueInput make_input(double mid_cents,
                                  double time_to_close_hours = kLargeTimeHours,
                                  int net_position = 0) {
  return kalshi::FairValueInput{
      mid_cents, time_to_close_hours, net_position, {}};
}

} // namespace

TEST(FairValueEngineTest, ReturnsMidPriceAtBaselineConditions) {
  auto engine = make_engine();
  EXPECT_NEAR(engine.estimate(make_input(kTestPriceCents)), kTestPriceCents,
              kTolerance);
}

TEST(FairValueEngineTest, TimeDecayPullsHighPriceTowardFifty) {
  auto engine = make_engine();
  const double result =
      engine.estimate(make_input(kHighPriceCents, kSmallTimeHours));
  EXPECT_LT(result, kHighPriceCents);
}

TEST(FairValueEngineTest, TimeDecayPullsLowPriceTowardFifty) {
  auto engine = make_engine();
  const double result =
      engine.estimate(make_input(kLowPriceCents, kSmallTimeHours));
  EXPECT_GT(result, kLowPriceCents);
}
