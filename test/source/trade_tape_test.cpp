#include "trade_tape.hpp"

#include "types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

const std::string kTicker = "KXMLB";
const std::string kOtherTicker = "KXNFL";

constexpr int kWindowSeconds = 300;
constexpr std::chrono::seconds kHalfLife{60};
constexpr int kLowPrice = 60;
constexpr int kHighPrice = 70;
constexpr int kSmallCount = 10;
constexpr int kLargeCount = 30;
constexpr double kVolumeWeightedMix = 67.5;
constexpr double kDecayedMix = 2000.0 / 30.0;
constexpr double kBalancedRatio = 0.5;
constexpr double kQuarterRatio = 0.25;
constexpr double kTolerance = 1e-9;
constexpr int kBeyondWindowSeconds = kWindowSeconds + 1;

kalshi::TradeTapeConfig test_config() {
  kalshi::TradeTapeConfig config;
  config.window_seconds = kWindowSeconds;
  return config;
}

kalshi::TradeTape::TimePoint base_time() {
  constexpr long long kBaseEpochSeconds = 1'700'000'000;
  return kalshi::TradeTape::TimePoint{} +
         std::chrono::seconds{kBaseEpochSeconds};
}

kalshi::PublicTrade make_trade(const std::string &trade_id, int yes_price_cents,
                               int contracts, kalshi::Side taker_side,
                               kalshi::TradeTape::TimePoint when,
                               const std::string &ticker = kTicker) {
  kalshi::PublicTrade trade;
  trade.trade_id = trade_id;
  trade.market_ticker = ticker;
  trade.yes_price_cents = yes_price_cents;
  trade.quantity = kalshi::Quantity::from_contracts(contracts);
  trade.taker_side = taker_side;
  trade.timestamp = when;
  return trade;
}

} // namespace

TEST(TradeTapeTest, EmptyTapeHasNoVwapNoPrintsNoRatio) {
  const kalshi::TradeTape tape{test_config()};
  const auto now = base_time();

  EXPECT_FALSE(tape.vwap_cents(kTicker, kHalfLife, now).has_value());
  EXPECT_EQ(tape.print_count(kTicker, now), 0);
  EXPECT_FALSE(tape.minority_side_ratio(kTicker, now).has_value());
}

TEST(TradeTapeTest, SingleTradeVwapIsItsPrice) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(
      make_trade("t1", kHighPrice, kSmallCount, kalshi::Side::Yes, now));

  const auto vwap = tape.vwap_cents(kTicker, kHalfLife, now);
  ASSERT_TRUE(vwap.has_value());
  EXPECT_NEAR(*vwap, kHighPrice, kTolerance);
}

TEST(TradeTapeTest, VwapWeightsByVolume) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(
      make_trade("t1", kLowPrice, kSmallCount, kalshi::Side::Yes, now));
  tape.record_trade(
      make_trade("t2", kHighPrice, kLargeCount, kalshi::Side::Yes, now));

  const auto vwap = tape.vwap_cents(kTicker, kHalfLife, now);
  ASSERT_TRUE(vwap.has_value());
  EXPECT_NEAR(*vwap, kVolumeWeightedMix, kTolerance);
}

TEST(TradeTapeTest, VwapDecaysOldPrintsByHalfLife) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(make_trade("t1", kLowPrice, kSmallCount, kalshi::Side::Yes,
                               now - kHalfLife));
  tape.record_trade(
      make_trade("t2", kHighPrice, kSmallCount, kalshi::Side::Yes, now));

  const auto vwap = tape.vwap_cents(kTicker, kHalfLife, now);
  ASSERT_TRUE(vwap.has_value());
  EXPECT_NEAR(*vwap, kDecayedMix, kTolerance);
}

TEST(TradeTapeTest, PrintsOutsideWindowIgnored) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(
      make_trade("t1", kLowPrice, kSmallCount, kalshi::Side::Yes,
                 now - std::chrono::seconds{kBeyondWindowSeconds}));
  tape.record_trade(
      make_trade("t2", kHighPrice, kSmallCount, kalshi::Side::Yes, now));

  const auto vwap = tape.vwap_cents(kTicker, kHalfLife, now);
  ASSERT_TRUE(vwap.has_value());
  EXPECT_NEAR(*vwap, kHighPrice, kTolerance);
  EXPECT_EQ(tape.print_count(kTicker, now), 1);
}

TEST(TradeTapeTest, OwnFillExcludedWhenRecordedBeforeTrade) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_own_fill("ours");
  tape.record_trade(
      make_trade("ours", kLowPrice, kSmallCount, kalshi::Side::Yes, now));
  tape.record_trade(
      make_trade("theirs", kHighPrice, kSmallCount, kalshi::Side::Yes, now));

  const auto vwap = tape.vwap_cents(kTicker, kHalfLife, now);
  ASSERT_TRUE(vwap.has_value());
  EXPECT_NEAR(*vwap, kHighPrice, kTolerance);
}

TEST(TradeTapeTest, OwnFillExcludedWhenRecordedAfterTrade) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(
      make_trade("ours", kLowPrice, kSmallCount, kalshi::Side::Yes, now));
  tape.record_trade(
      make_trade("theirs", kHighPrice, kSmallCount, kalshi::Side::Yes, now));
  tape.record_own_fill("ours");

  const auto vwap = tape.vwap_cents(kTicker, kHalfLife, now);
  ASSERT_TRUE(vwap.has_value());
  EXPECT_NEAR(*vwap, kHighPrice, kTolerance);
}

TEST(TradeTapeTest, PrintCountExcludesOwnFills) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_own_fill("ours");
  tape.record_trade(
      make_trade("ours", kLowPrice, kSmallCount, kalshi::Side::Yes, now));
  tape.record_trade(
      make_trade("theirs", kHighPrice, kSmallCount, kalshi::Side::Yes, now));

  EXPECT_EQ(tape.print_count(kTicker, now), 1);
}

TEST(TradeTapeTest, TradesForOtherTickerDoNotMix) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(
      make_trade("t1", kLowPrice, kSmallCount, kalshi::Side::Yes, now));
  tape.record_trade(make_trade("t2", kHighPrice, kSmallCount, kalshi::Side::Yes,
                               now, kOtherTicker));

  const auto vwap = tape.vwap_cents(kTicker, kHalfLife, now);
  ASSERT_TRUE(vwap.has_value());
  EXPECT_NEAR(*vwap, kLowPrice, kTolerance);
  EXPECT_EQ(tape.print_count(kOtherTicker, now), 1);
}

TEST(TradeTapeTest, EmptyOwnFillIdExcludesNothing) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_own_fill("");
  tape.record_trade(
      make_trade("", kHighPrice, kSmallCount, kalshi::Side::Yes, now));

  EXPECT_EQ(tape.print_count(kTicker, now), 1);
}

TEST(TradeTapeTest, MinorityRatioBalancedFlowIsHalf) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(
      make_trade("t1", kHighPrice, kSmallCount, kalshi::Side::Yes, now));
  tape.record_trade(
      make_trade("t2", kHighPrice, kSmallCount, kalshi::Side::No, now));

  const auto ratio = tape.minority_side_ratio(kTicker, now);
  ASSERT_TRUE(ratio.has_value());
  EXPECT_NEAR(*ratio, kBalancedRatio, kTolerance);
}

TEST(TradeTapeTest, MinorityRatioOneWayFlowIsZero) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(
      make_trade("t1", kHighPrice, kSmallCount, kalshi::Side::Yes, now));
  tape.record_trade(
      make_trade("t2", kHighPrice, kLargeCount, kalshi::Side::Yes, now));

  const auto ratio = tape.minority_side_ratio(kTicker, now);
  ASSERT_TRUE(ratio.has_value());
  EXPECT_NEAR(*ratio, 0.0, kTolerance);
}

TEST(TradeTapeTest, MinorityRatioWeightsByVolume) {
  kalshi::TradeTape tape{test_config()};
  const auto now = base_time();
  tape.record_trade(
      make_trade("t1", kHighPrice, kLargeCount, kalshi::Side::Yes, now));
  tape.record_trade(
      make_trade("t2", kHighPrice, kSmallCount, kalshi::Side::No, now));

  const auto ratio = tape.minority_side_ratio(kTicker, now);
  ASSERT_TRUE(ratio.has_value());
  EXPECT_NEAR(*ratio, kQuarterRatio, kTolerance);
}
