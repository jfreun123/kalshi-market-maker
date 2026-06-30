#include "portfolio.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

// Minimal IOrderManager fake: returns values from settable per-ticker maps so
// the Portfolio read-model can be tested in isolation. A struct (passive data
// holder) so its maps are intentionally public for direct test setup.
struct FakeOrderManager : public kalshi::IOrderManager {
  std::unordered_map<std::string, int> positions;
  std::unordered_map<std::string, double> realized;
  std::unordered_map<std::string, double> unrealized;
  std::unordered_map<std::string, double> cost;
  std::unordered_map<std::string, kalshi::Order> open_map;

  kalshi::Order place(std::string_view /*ticker*/, kalshi::Side /*side*/,
                      int /*price_cents*/, int /*quantity*/) override {
    return {};
  }
  bool cancel(std::string_view /*order_id*/) override { return true; }
  void cancel_all(std::string_view /*ticker*/) override {}
  void record_fill(const kalshi::Fill & /*fill*/) override {}

  [[nodiscard]] int net_position(std::string_view ticker) const override {
    auto iter = positions.find(std::string{ticker});
    return iter == positions.end() ? 0 : iter->second;
  }
  [[nodiscard]] double realized_pnl(std::string_view ticker) const override {
    auto iter = realized.find(std::string{ticker});
    return iter == realized.end() ? 0.0 : iter->second;
  }
  [[nodiscard]] double unrealized_pnl(std::string_view ticker,
                                      int /*yes_mid_cents*/) const override {
    auto iter = unrealized.find(std::string{ticker});
    return iter == unrealized.end() ? 0.0 : iter->second;
  }
  [[nodiscard]] double position_cost(std::string_view ticker) const override {
    auto iter = cost.find(std::string{ticker});
    return iter == cost.end() ? 0.0 : iter->second;
  }
  [[nodiscard]] kalshi::ExposureDecomposition
  exposure_decomposition(std::string_view /*ticker*/) const override {
    return {}; // not exercised by the Portfolio aggregation tests
  }
  [[nodiscard]] const std::unordered_map<std::string, kalshi::Order> &
  open_orders() const override {
    return open_map;
  }
};

// Named values for the aggregation tests.
constexpr double kRealizedA = 100.0;
constexpr double kRealizedB = 50.0;
constexpr double kUnrealizedA = 20.0;
constexpr double kUnrealizedB = -5.0;
constexpr double kCostA = 260.0;
constexpr double kCostB = 130.0;
constexpr int kMidA = 60;
constexpr int kMidB = 40;

constexpr double kEventRealizedLow = 10.0;
constexpr double kEventRealizedHigh = 30.0;
constexpr double kEventCostLow = 100.0;
constexpr double kEventCostHigh = 200.0;

constexpr double kNotionalSmall = 50.0;
constexpr double kNotionalMid = 500.0;
constexpr double kNotionalBig = 5000.0;

} // namespace

// ---- event_ticker_of ----

TEST(PortfolioTest, EventTickerStripsFinalStrikeSegment) {
  EXPECT_EQ(kalshi::event_ticker_of("KXFED-26SEP-T3.00"), "KXFED-26SEP");
  EXPECT_EQ(kalshi::event_ticker_of("KXNEXTTEAMNBA-26JBROWN7-CLE"),
            "KXNEXTTEAMNBA-26JBROWN7");
}

TEST(PortfolioTest, EventTickerOfTickerWithoutHyphenIsItself) {
  EXPECT_EQ(kalshi::event_ticker_of("SINGLE"), "SINGLE");
}

// ---- snapshot aggregation ----

TEST(PortfolioTest, EmptyUniverseProducesZeroSnapshot) {
  const FakeOrderManager order_mgr;
  const kalshi::Portfolio portfolio{order_mgr};

  const auto snap = portfolio.snapshot({}, {});

  EXPECT_DOUBLE_EQ(snap.total_realized_cents, 0.0);
  EXPECT_DOUBLE_EQ(snap.total_unrealized_cents, 0.0);
  EXPECT_DOUBLE_EQ(snap.total_notional_cents, 0.0);
  EXPECT_TRUE(snap.by_event.empty());
}

TEST(PortfolioTest, SumsRealizedUnrealizedAndNotionalAcrossTickers) {
  FakeOrderManager order_mgr;
  order_mgr.realized["KXA-26-T1"] = kRealizedA;
  order_mgr.realized["KXB-26-T1"] = kRealizedB;
  order_mgr.unrealized["KXA-26-T1"] = kUnrealizedA;
  order_mgr.unrealized["KXB-26-T1"] = kUnrealizedB;
  order_mgr.cost["KXA-26-T1"] = kCostA;
  order_mgr.cost["KXB-26-T1"] = kCostB;

  const kalshi::Portfolio portfolio{order_mgr};
  const kalshi::MarkMap marks = {{"KXA-26-T1", kMidA}, {"KXB-26-T1", kMidB}};

  const auto snap = portfolio.snapshot({"KXA-26-T1", "KXB-26-T1"}, marks);

  EXPECT_DOUBLE_EQ(snap.total_realized_cents, kRealizedA + kRealizedB);
  EXPECT_DOUBLE_EQ(snap.total_unrealized_cents, kUnrealizedA + kUnrealizedB);
  EXPECT_DOUBLE_EQ(snap.total_notional_cents, kCostA + kCostB);
  EXPECT_DOUBLE_EQ(snap.total_pnl_cents(),
                   kRealizedA + kRealizedB + kUnrealizedA + kUnrealizedB);
}

TEST(PortfolioTest, GroupsCorrelatedStrikesUnderOneEvent) {
  FakeOrderManager order_mgr;
  order_mgr.realized["KXCPI-26SEP-T3.0"] = kEventRealizedLow;
  order_mgr.realized["KXCPI-26SEP-T3.5"] = kEventRealizedHigh;
  order_mgr.cost["KXCPI-26SEP-T3.0"] = kEventCostLow;
  order_mgr.cost["KXCPI-26SEP-T3.5"] = kEventCostHigh;

  const kalshi::Portfolio portfolio{order_mgr};
  const auto snap =
      portfolio.snapshot({"KXCPI-26SEP-T3.0", "KXCPI-26SEP-T3.5"}, {});

  ASSERT_EQ(snap.by_event.size(), 1U);
  EXPECT_EQ(snap.by_event.at(0).event_ticker, "KXCPI-26SEP");
  EXPECT_EQ(snap.by_event.at(0).market_count, 2);
  EXPECT_DOUBLE_EQ(snap.by_event.at(0).realized_pnl_cents,
                   kEventRealizedLow + kEventRealizedHigh);
  EXPECT_DOUBLE_EQ(snap.by_event.at(0).notional_cost_cents,
                   kEventCostLow + kEventCostHigh);
}

TEST(PortfolioTest, ByEventSortedByNotionalDescending) {
  FakeOrderManager order_mgr;
  order_mgr.cost["KXSMALL-26-T1"] = kNotionalSmall;
  order_mgr.cost["KXBIG-26-T1"] = kNotionalBig;
  order_mgr.cost["KXMID-26-T1"] = kNotionalMid;

  const kalshi::Portfolio portfolio{order_mgr};
  const auto snap =
      portfolio.snapshot({"KXSMALL-26-T1", "KXBIG-26-T1", "KXMID-26-T1"}, {});

  ASSERT_EQ(snap.by_event.size(), 3U);
  EXPECT_EQ(snap.by_event.at(0).event_ticker, "KXBIG-26");
  EXPECT_EQ(snap.by_event.at(1).event_ticker, "KXMID-26");
  EXPECT_EQ(snap.by_event.at(2).event_ticker, "KXSMALL-26");
}

// ---- reconcile ----

namespace {
kalshi::MarketPosition exchange_pos(const std::string &ticker, int position) {
  kalshi::MarketPosition pos;
  pos.ticker = ticker;
  pos.position = position;
  return pos;
}

constexpr int kLocalYes = 5;
constexpr int kLocalNo = -3;
constexpr int kExchangeMismatch = 3;
constexpr int kUntrackedPosition = 4;
} // namespace

TEST(PortfolioTest, ReconcileInSyncWhenPositionsMatch) {
  FakeOrderManager order_mgr;
  order_mgr.positions["KXA-26-T1"] = kLocalYes;
  order_mgr.positions["KXB-26-T1"] = kLocalNo;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXA-26-T1", kLocalYes),
      exchange_pos("KXB-26-T1", kLocalNo)};

  const auto result =
      kalshi::reconcile(order_mgr, {"KXA-26-T1", "KXB-26-T1"}, exchange);

  EXPECT_TRUE(result.in_sync);
  EXPECT_TRUE(result.diffs.empty());
}

TEST(PortfolioTest, ReconcileDetectsPositionMismatch) {
  FakeOrderManager order_mgr;
  order_mgr.positions["KXA-26-T1"] = kLocalYes;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXA-26-T1", kExchangeMismatch)};

  const auto result = kalshi::reconcile(order_mgr, {"KXA-26-T1"}, exchange);

  EXPECT_FALSE(result.in_sync);
  ASSERT_EQ(result.diffs.size(), 1U);
  EXPECT_EQ(result.diffs.at(0).ticker, "KXA-26-T1");
  EXPECT_EQ(result.diffs.at(0).local_position, kLocalYes);
  EXPECT_EQ(result.diffs.at(0).exchange_position, kExchangeMismatch);
}

TEST(PortfolioTest, ReconcileFlagsUntrackedExchangePosition) {
  // The exchange reports a non-zero position in a ticker we are not tracking —
  // a position we don't know about. This must be flagged.
  FakeOrderManager order_mgr;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXSURPRISE-26-T1", kUntrackedPosition)};

  const auto result = kalshi::reconcile(order_mgr, {}, exchange);

  EXPECT_FALSE(result.in_sync);
  ASSERT_EQ(result.diffs.size(), 1U);
  EXPECT_EQ(result.diffs.at(0).ticker, "KXSURPRISE-26-T1");
  EXPECT_EQ(result.diffs.at(0).local_position, 0);
  EXPECT_EQ(result.diffs.at(0).exchange_position, kUntrackedPosition);
}

TEST(PortfolioTest, ReconcileIgnoresZeroExchangePositions) {
  // Settled/flat markets the exchange still lists with position 0 must not be
  // treated as drift.
  FakeOrderManager order_mgr;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXOLD-26-T1", 0)};

  const auto result = kalshi::reconcile(order_mgr, {}, exchange);

  EXPECT_TRUE(result.in_sync);
  EXPECT_TRUE(result.diffs.empty());
}
