#include "analytics.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace {

constexpr long long kFixedEpochMs = 1'783'125'395'000LL;
constexpr double kMid = 52.5;
constexpr double kMicro = 52.8;
constexpr double kFairValue = 53.1;
constexpr int kBid = 51;
constexpr int kAsk = 55;
constexpr double kInventory = -10.0;
constexpr int kFillPrice = 65;
constexpr int kFillQty = 3;
constexpr double kInventoryAfter = -13.0;

const std::string kTicker = "KXTEST-MARKET";

struct CollectingSink {
  std::vector<std::string> lines;
  kalshi::AnalyticsLogger::Sink as_sink() {
    return [this](const std::string &line) { lines.push_back(line); };
  }
};

kalshi::AnalyticsLogger::Clock fixed_clock() {
  return [] {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{kFixedEpochMs}};
  };
}

kalshi::Fill make_fill() {
  kalshi::Fill fill;
  fill.trade_id = "trade-1";
  fill.order_id = "order-1";
  fill.market_ticker = kTicker;
  fill.side = kalshi::Side::No;
  fill.price_cents = kFillPrice;
  fill.quantity = kalshi::Quantity::from_contracts(kFillQty);
  fill.is_taker = false;
  fill.timestamp = std::chrono::system_clock::time_point{
      std::chrono::milliseconds{kFixedEpochMs}};
  return fill;
}

} // namespace

TEST(AnalyticsLoggerTest, QuoteDecisionEmitsOneParseableJsonLine) {
  CollectingSink sink;
  kalshi::AnalyticsLogger analytics{sink.as_sink(), fixed_clock()};

  analytics.quote_decision(
      {kTicker, kMid, kMicro, kFairValue, kBid, kAsk, kInventory, true});

  ASSERT_EQ(sink.lines.size(), 1U);
  const auto parsed = nlohmann::json::parse(sink.lines.front());
  EXPECT_EQ(parsed.at("type"), "quote");
  EXPECT_EQ(parsed.at("ts_ms"), kFixedEpochMs);
  EXPECT_EQ(parsed.at("ticker"), kTicker);
  EXPECT_DOUBLE_EQ(parsed.at("mid"), kMid);
  EXPECT_DOUBLE_EQ(parsed.at("micro"), kMicro);
  EXPECT_DOUBLE_EQ(parsed.at("fv"), kFairValue);
  EXPECT_EQ(parsed.at("bid"), kBid);
  EXPECT_EQ(parsed.at("ask"), kAsk);
  EXPECT_DOUBLE_EQ(parsed.at("inventory"), kInventory);
  EXPECT_EQ(parsed.at("imbalanced"), true);
}

TEST(AnalyticsLoggerTest, FillEmitsJsonLineWithMarketContext) {
  CollectingSink sink;
  kalshi::AnalyticsLogger analytics{sink.as_sink(), fixed_clock()};

  analytics.fill(make_fill(), kMid, kInventoryAfter);

  ASSERT_EQ(sink.lines.size(), 1U);
  const auto parsed = nlohmann::json::parse(sink.lines.front());
  EXPECT_EQ(parsed.at("type"), "fill");
  EXPECT_EQ(parsed.at("ts_ms"), kFixedEpochMs);
  EXPECT_EQ(parsed.at("ticker"), kTicker);
  EXPECT_EQ(parsed.at("trade_id"), "trade-1");
  EXPECT_EQ(parsed.at("side"), "no");
  EXPECT_EQ(parsed.at("price"), kFillPrice);
  EXPECT_DOUBLE_EQ(parsed.at("qty"), static_cast<double>(kFillQty));
  EXPECT_EQ(parsed.at("is_taker"), false);
  EXPECT_DOUBLE_EQ(parsed.at("mid"), kMid);
  EXPECT_DOUBLE_EQ(parsed.at("inventory_after"), kInventoryAfter);
}

TEST(AnalyticsLoggerTest, NullSinkIsSafeNoOp) {
  kalshi::AnalyticsLogger analytics{{}, fixed_clock()};

  analytics.quote_decision(
      {kTicker, kMid, kMicro, kFairValue, kBid, kAsk, kInventory, false});
  analytics.fill(make_fill(), kMid, kInventoryAfter);
}
