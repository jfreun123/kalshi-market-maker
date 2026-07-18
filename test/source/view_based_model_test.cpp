#include "engine/pricing_model.hpp"

#include <gtest/gtest.h>

#include <optional>

// ---- Test constants ----

namespace {

constexpr double kTimeHours = 1'000.0; // ignored by the model; any value
constexpr int kFlatPosition = 0;

constexpr double kMidCents = 50.0;         // unbiased point
constexpr double kLongshotMidCents = 20.0; // → debiased ~17.03 (moves down)
constexpr double kFavoriteMidCents = 80.0; // → debiased ~82.97 (moves up)
constexpr double kExtremeLongCents = 5.0;  // debias < 1% → clamps to 1c
constexpr double kExtremeFavCents = 96.0;  // debias > 99% → clamps to 99c

constexpr double kExpectedLongshot = 17.033; // (0.20-0.045)/0.91 * 100
constexpr double kExpectedFavorite = 82.967; // (0.80-0.045)/0.91 * 100
constexpr double kFloorCents = 1.0;
constexpr double kCeilCents = 99.0;
constexpr double kTolerance = 0.05;

constexpr double kExternalProb = 0.30; // injected view
constexpr double kExpectedExternalCents = 30.0;

kalshi::FairValueInput
input(double mid_cents, std::optional<double> external_prob = std::nullopt) {
  return kalshi::FairValueInput{mid_cents, kTimeHours, kFlatPosition,
                                external_prob};
}

} // namespace

TEST(ViewBasedModelTest, MidpointIsUnbiased) {
  const kalshi::ViewBasedModel model; // default β = 0.09
  EXPECT_NEAR(model.estimate(input(kMidCents)), kMidCents, kTolerance);
}

TEST(ViewBasedModelTest, LongshotIsDebiasedDown) {
  const kalshi::ViewBasedModel model;
  const double value = model.estimate(input(kLongshotMidCents));
  EXPECT_NEAR(value, kExpectedLongshot, kTolerance);
  EXPECT_LT(value, kLongshotMidCents); // market overprices longshots
}

TEST(ViewBasedModelTest, FavoriteIsDebiasedUp) {
  const kalshi::ViewBasedModel model;
  const double value = model.estimate(input(kFavoriteMidCents));
  EXPECT_NEAR(value, kExpectedFavorite, kTolerance);
  EXPECT_GT(value, kFavoriteMidCents); // market underprices favorites
}

TEST(ViewBasedModelTest, ExtremeLongshotClampsToFloor) {
  const kalshi::ViewBasedModel model;
  EXPECT_NEAR(model.estimate(input(kExtremeLongCents)), kFloorCents,
              kTolerance);
}

TEST(ViewBasedModelTest, ExtremeFavoriteClampsToCeiling) {
  const kalshi::ViewBasedModel model;
  EXPECT_NEAR(model.estimate(input(kExtremeFavCents)), kCeilCents, kTolerance);
}

TEST(ViewBasedModelTest, ExternalViewOverridesMarketMid) {
  const kalshi::ViewBasedModel model;
  // External view of 30% is used regardless of the (favorite) market mid.
  EXPECT_NEAR(model.estimate(input(kFavoriteMidCents, kExternalProb)),
              kExpectedExternalCents, kTolerance);
}

TEST(ViewBasedModelTest, DebiasProbabilityHelperMatchesFormula) {
  constexpr double kBeta = 0.09;
  constexpr double kMarketProb = 0.20;
  constexpr double kExpectedProb = 0.17033;
  EXPECT_NEAR(kalshi::debias_probability(kMarketProb, kBeta), kExpectedProb,
              kTolerance / 100.0);
}
