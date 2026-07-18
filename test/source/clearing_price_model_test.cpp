#include "engine/pricing_model.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace {

constexpr double kTimeHours = 1'000.0;
constexpr int kFlatPosition = 0;

constexpr double kAnchorCents = 62.0;
constexpr double kTapeVwapCents = 64.0;
constexpr double kHalfWeight = 0.5;
constexpr double kQuarterWeight = 0.25;
constexpr double kPureTapeWeight = 1.0;
constexpr double kHalfBlend = 63.0;
constexpr double kQuarterBlend = 62.5;
constexpr double kFloorCents = 1.0;
constexpr double kBelowFloorVwap = 0.0;
constexpr double kTolerance = 1e-9;

kalshi::FairValueInput input(double mid_cents,
                             std::optional<double> tape_vwap = std::nullopt) {
  return kalshi::FairValueInput{
      .mid_cents = mid_cents,
      .time_to_close_hours = kTimeHours,
      .net_position = kFlatPosition,
      .external_prob = std::nullopt,
      .tape_vwap_cents = tape_vwap,
  };
}

} // namespace

TEST(ClearingPriceModelTest, NoTapeFallsBackToAnchor) {
  const kalshi::ClearingPriceModel model{kHalfWeight};
  EXPECT_NEAR(model.estimate(input(kAnchorCents)), kAnchorCents, kTolerance);
}

TEST(ClearingPriceModelTest, BlendsAnchorTowardTapeVwap) {
  const kalshi::ClearingPriceModel model{kHalfWeight};
  EXPECT_NEAR(model.estimate(input(kAnchorCents, kTapeVwapCents)), kHalfBlend,
              kTolerance);
}

TEST(ClearingPriceModelTest, QuarterWeightLeansLightly) {
  const kalshi::ClearingPriceModel model{kQuarterWeight};
  EXPECT_NEAR(model.estimate(input(kAnchorCents, kTapeVwapCents)),
              kQuarterBlend, kTolerance);
}

TEST(ClearingPriceModelTest, PureTapeWeightReturnsVwap) {
  const kalshi::ClearingPriceModel model{kPureTapeWeight};
  EXPECT_NEAR(model.estimate(input(kAnchorCents, kTapeVwapCents)),
              kTapeVwapCents, kTolerance);
}

TEST(ClearingPriceModelTest, ClampsToValidPriceRange) {
  const kalshi::ClearingPriceModel model{kPureTapeWeight};
  EXPECT_NEAR(model.estimate(input(kAnchorCents, kBelowFloorVwap)), kFloorCents,
              kTolerance);
}
