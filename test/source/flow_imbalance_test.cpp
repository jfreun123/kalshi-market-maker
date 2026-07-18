#include "engine/flow_imbalance.hpp"

#include "core/types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

const std::string kTicker = "KXBTCD";

constexpr int kWindowSeconds = 60;
constexpr double kRatioThreshold = 2.0;
constexpr int kMinFlowVolume = 20;

constexpr kalshi::Quantity kBalancedQty = kalshi::Quantity::from_contracts(15);
constexpr kalshi::Quantity kHeavyYesQty = kalshi::Quantity::from_contracts(30);
constexpr kalshi::Quantity kLightNoQty = kalshi::Quantity::from_contracts(5);
constexpr kalshi::Quantity kModerateNoQty =
    kalshi::Quantity::from_contracts(10);
constexpr kalshi::Quantity kThinQty = kalshi::Quantity::from_contracts(8);
constexpr double kExpectedRatio3 = 3.0;
constexpr int kBeyondWindowSeconds = 120;

kalshi::FlowImbalanceConfig test_config() {
  kalshi::FlowImbalanceConfig config;
  config.window_seconds = kWindowSeconds;
  config.imbalance_ratio_threshold = kRatioThreshold;
  config.min_flow_volume = kMinFlowVolume;
  return config;
}

// A fixed reference time so windowing is deterministic.
kalshi::FlowImbalanceGuard::TimePoint base_time() {
  constexpr long long kBaseEpochSeconds = 1'700'000'000;
  return kalshi::FlowImbalanceGuard::TimePoint{} +
         std::chrono::seconds{kBaseEpochSeconds};
}

} // namespace

TEST(FlowImbalanceGuardTest, NoFlowIsBalanced) {
  const kalshi::FlowImbalanceGuard guard{test_config()};
  EXPECT_DOUBLE_EQ(guard.imbalance_ratio(kTicker, base_time()), 1.0);
  EXPECT_FALSE(guard.is_imbalanced(kTicker, base_time()));
}

TEST(FlowImbalanceGuardTest, BalancedFlowIsNotImbalanced) {
  kalshi::FlowImbalanceGuard guard{test_config()};
  guard.record_fill(kTicker, kalshi::Side::Yes, kBalancedQty, base_time());
  guard.record_fill(kTicker, kalshi::Side::No, kBalancedQty, base_time());

  EXPECT_DOUBLE_EQ(guard.imbalance_ratio(kTicker, base_time()), 1.0);
  EXPECT_FALSE(guard.is_imbalanced(kTicker, base_time()));
}

TEST(FlowImbalanceGuardTest, OneSidedFlowIsImbalanced) {
  kalshi::FlowImbalanceGuard guard{test_config()};
  guard.record_fill(kTicker, kalshi::Side::Yes, kHeavyYesQty, base_time());
  guard.record_fill(kTicker, kalshi::Side::No, kLightNoQty, base_time());

  // 30/5 = 6.0 > 2.0 threshold, and 35 ≥ 20 min volume.
  EXPECT_TRUE(guard.is_imbalanced(kTicker, base_time()));
}

TEST(FlowImbalanceGuardTest, RatioIsLargerOverSmallerSide) {
  kalshi::FlowImbalanceGuard guard{test_config()};
  guard.record_fill(kTicker, kalshi::Side::Yes, kHeavyYesQty, base_time());
  guard.record_fill(kTicker, kalshi::Side::No, kModerateNoQty, base_time());

  EXPECT_DOUBLE_EQ(guard.imbalance_ratio(kTicker, base_time()),
                   kExpectedRatio3); // 30 / 10
}

TEST(FlowImbalanceGuardTest, ThinFlowIsNotImbalancedDespiteOneSided) {
  kalshi::FlowImbalanceGuard guard{test_config()};
  guard.record_fill(kTicker, kalshi::Side::Yes, kThinQty, base_time());

  // Fully one-sided, but 8 < 20 min volume → not enough signal to act.
  EXPECT_FALSE(guard.is_imbalanced(kTicker, base_time()));
}

TEST(FlowImbalanceGuardTest, FillsOutsideWindowAreIgnored) {
  kalshi::FlowImbalanceGuard guard{test_config()};
  guard.record_fill(kTicker, kalshi::Side::Yes, kHeavyYesQty, base_time());

  // Query 120s later with a 60s window → the old flow has aged out.
  const auto later = base_time() + std::chrono::seconds{kBeyondWindowSeconds};
  EXPECT_DOUBLE_EQ(guard.imbalance_ratio(kTicker, later), 1.0);
  EXPECT_FALSE(guard.is_imbalanced(kTicker, later));
}

TEST(FlowImbalanceGuardTest, ResetClearsTicker) {
  kalshi::FlowImbalanceGuard guard{test_config()};
  guard.record_fill(kTicker, kalshi::Side::Yes, kHeavyYesQty, base_time());
  guard.record_fill(kTicker, kalshi::Side::No, kLightNoQty, base_time());
  ASSERT_TRUE(guard.is_imbalanced(kTicker, base_time()));

  guard.reset(kTicker);

  EXPECT_FALSE(guard.is_imbalanced(kTicker, base_time()));
  EXPECT_DOUBLE_EQ(guard.imbalance_ratio(kTicker, base_time()), 1.0);
}
