#include "engine/portfolio.hpp"

#include "fake_order_manager.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

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
kalshi::MarketPosition exchange_pos(const std::string &ticker,
                                    kalshi::Quantity position) {
  kalshi::MarketPosition pos;
  pos.ticker = ticker;
  pos.position = position;
  return pos;
}

constexpr kalshi::Quantity kLocalYes = kalshi::Quantity::from_contracts(5);
constexpr kalshi::Quantity kLocalNo = kalshi::Quantity::from_contracts(-3);
constexpr kalshi::Quantity kExchangeMismatch =
    kalshi::Quantity::from_contracts(3);
constexpr kalshi::Quantity kUntrackedPosition =
    kalshi::Quantity::from_contracts(4);
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
  EXPECT_EQ(result.diffs.at(0).local_position, kalshi::Quantity{});
  EXPECT_EQ(result.diffs.at(0).exchange_position, kUntrackedPosition);
}

TEST(PortfolioTest, ReconcileIgnoresZeroExchangePositions) {
  // Settled/flat markets the exchange still lists with position 0 must not be
  // treated as drift.
  FakeOrderManager order_mgr;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXOLD-26-T1", kalshi::Quantity{})};

  const auto result = kalshi::reconcile(order_mgr, {}, exchange);

  EXPECT_TRUE(result.in_sync);
  EXPECT_TRUE(result.diffs.empty());
}

namespace {
constexpr kalshi::Quantity kLeftoverShort =
    kalshi::Quantity::from_contracts(-10);
constexpr kalshi::Quantity kLeftoverPartial =
    kalshi::Quantity::from_contracts(-7);
} // namespace

TEST(PortfolioTest, ReconcileBaselinedLeftoverInUntradedMarketIsNotDrift) {
  FakeOrderManager order_mgr;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXLEFTOVER-26-T1", kLeftoverShort)};
  const std::vector<kalshi::MarketPosition> baseline = {
      exchange_pos("KXLEFTOVER-26-T1", kLeftoverShort)};

  const auto result = kalshi::reconcile(order_mgr, {}, exchange, baseline);

  EXPECT_TRUE(result.in_sync);
  EXPECT_TRUE(result.diffs.empty());
}

TEST(PortfolioTest, ReconcileDetectsChangeFromBaselinedLeftover) {
  FakeOrderManager order_mgr;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXLEFTOVER-26-T1", kLeftoverPartial)};
  const std::vector<kalshi::MarketPosition> baseline = {
      exchange_pos("KXLEFTOVER-26-T1", kLeftoverShort)};

  const auto result = kalshi::reconcile(order_mgr, {}, exchange, baseline);

  EXPECT_FALSE(result.in_sync);
  ASSERT_EQ(result.diffs.size(), 1U);
  EXPECT_EQ(result.diffs.at(0).ticker, "KXLEFTOVER-26-T1");
  EXPECT_EQ(result.diffs.at(0).exchange_position, kLeftoverPartial);
  EXPECT_EQ(result.diffs.at(0).baseline_position, kLeftoverShort);
}

TEST(PortfolioTest, ReconcileVanishedBaselinePositionIsDrift) {
  FakeOrderManager order_mgr;

  const std::vector<kalshi::MarketPosition> baseline = {
      exchange_pos("KXSETTLED-26-T1", kLeftoverShort)};

  const auto result = kalshi::reconcile(order_mgr, {}, {}, baseline);

  EXPECT_FALSE(result.in_sync);
  ASSERT_EQ(result.diffs.size(), 1U);
  EXPECT_EQ(result.diffs.at(0).ticker, "KXSETTLED-26-T1");
  EXPECT_EQ(result.diffs.at(0).exchange_position, kalshi::Quantity{});
}

TEST(PortfolioTest, ReconcileBaselineDoesNotExcuseTradedTicker) {
  FakeOrderManager order_mgr;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXTRADED-26-T1", kLeftoverShort)};
  const std::vector<kalshi::MarketPosition> baseline = {
      exchange_pos("KXTRADED-26-T1", kLeftoverShort)};

  const auto result =
      kalshi::reconcile(order_mgr, {"KXTRADED-26-T1"}, exchange, baseline);

  EXPECT_FALSE(result.in_sync);
  ASSERT_EQ(result.diffs.size(), 1U);
  EXPECT_EQ(result.diffs.at(0).ticker, "KXTRADED-26-T1");
  EXPECT_EQ(result.diffs.at(0).local_position, kalshi::Quantity{});
  EXPECT_EQ(result.diffs.at(0).exchange_position, kLeftoverShort);
}

TEST(PortfolioTest, ReconcileTradedTickerFillsMatchWithUnrelatedBaseline) {
  FakeOrderManager order_mgr;
  order_mgr.positions["KXTRADED-26-T1"] = kLocalYes;

  const std::vector<kalshi::MarketPosition> exchange = {
      exchange_pos("KXTRADED-26-T1", kLocalYes),
      exchange_pos("KXLEFTOVER-26-T1", kLeftoverShort)};
  const std::vector<kalshi::MarketPosition> baseline = {
      exchange_pos("KXLEFTOVER-26-T1", kLeftoverShort)};

  const auto result =
      kalshi::reconcile(order_mgr, {"KXTRADED-26-T1"}, exchange, baseline);

  EXPECT_TRUE(result.in_sync);
  EXPECT_TRUE(result.diffs.empty());
}
