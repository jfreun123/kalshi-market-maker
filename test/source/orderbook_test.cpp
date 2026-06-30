#include "orderbook.hpp"

#include <gtest/gtest.h>

// ---- Test constants ----

namespace {

// A typical Kalshi market with a 4-cent spread:
//   YES bid 52, NO bid 44 => YES ask = 56, spread = 4, mid = 54
constexpr int kYesBid = 52;
constexpr int kYesBid2 = 51;
constexpr int kNoBid = 44;
constexpr int kNoBid2 = 43;
constexpr int kYesAsk = 56; // 100 - kNoBid
constexpr int kExpectedMid = 54;
constexpr int kExpectedSpread = 4;
constexpr int kQtyLarge = 200;
constexpr int kQtySmall = 100;
constexpr int kQtyUpdated = 75;
constexpr int kNewPrice = 50;
constexpr int kNewQty = 300;
constexpr std::size_t kOneLevel = 1U;
constexpr std::size_t kTwoLevels = 2U;
constexpr std::size_t kThreeLevels = 3U;

kalshi::Orderbook make_orderbook() {
  kalshi::Orderbook book;
  book.ticker = "KXBTCD";
  book.yes = {{kYesBid, kQtyLarge}, {kYesBid2, kQtySmall}};
  book.no = {{kNoBid, kQtyLarge}, {kNoBid2, kQtySmall}};
  return book;
}

} // namespace

// ---- Empty book ----

TEST(LocalOrderbookTest, EmptyBookBestBidReturnsNullopt) {
  kalshi::LocalOrderbook orderbook;
  EXPECT_FALSE(orderbook.best_bid().has_value());
}

TEST(LocalOrderbookTest, EmptyBookBestAskReturnsNullopt) {
  kalshi::LocalOrderbook orderbook;
  EXPECT_FALSE(orderbook.best_ask().has_value());
}

TEST(LocalOrderbookTest, EmptyBookMidPriceReturnsZero) {
  kalshi::LocalOrderbook orderbook;
  EXPECT_DOUBLE_EQ(orderbook.mid_price_cents(), 0.0);
}

TEST(LocalOrderbookTest, EmptyBookSpreadReturnsZero) {
  kalshi::LocalOrderbook orderbook;
  EXPECT_EQ(orderbook.spread_cents(), 0);
}

// ---- apply_snapshot ----

TEST(LocalOrderbookTest, ApplySnapshotSetsTicker) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());
  EXPECT_EQ(orderbook.state().ticker, "KXBTCD");
}

TEST(LocalOrderbookTest, ApplySnapshotSetsYesLevels) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());
  ASSERT_EQ(orderbook.state().yes.size(), kTwoLevels);
  EXPECT_EQ(orderbook.state().yes[0].price_cents, kYesBid);
  EXPECT_EQ(orderbook.state().yes[0].quantity, kQtyLarge);
}

TEST(LocalOrderbookTest, ApplySnapshotSetsNoLevels) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());
  ASSERT_EQ(orderbook.state().no.size(), kTwoLevels);
  EXPECT_EQ(orderbook.state().no[0].price_cents, kNoBid);
}

TEST(LocalOrderbookTest, ApplySnapshotOverwritesPreviousState) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  kalshi::Orderbook new_book;
  new_book.ticker = "OTHER";
  new_book.yes = {{kYesBid, kQtySmall}};
  orderbook.apply_snapshot(new_book);

  EXPECT_EQ(orderbook.state().ticker, "OTHER");
  ASSERT_EQ(orderbook.state().yes.size(), kOneLevel);
}

// ---- BBO after a snapshot in the exchange's ascending order ----

TEST(LocalOrderbookTest, ApplySnapshotSortsExchangeAscendingLevelsDescending) {
  // The exchange sends levels in ASCENDING price order (lowest first, typically
  // a 1c dust level). best_bid/best_ask/find_level all assume DESCENDING (best
  // at front), so apply_snapshot must sort — otherwise best_bid is the 1c dust
  // level and the computed mid is garbage (this caused post-only-cross quotes).
  kalshi::Orderbook book;
  book.ticker = "KXBTCD";
  book.yes = {{1, 5000}, {46, 100}, {53, 200}}; // ascending as on the wire
  book.no = {{1, 5000}, {40, 100}, {44, 300}};  // ascending as on the wire

  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(book);

  ASSERT_TRUE(orderbook.best_bid().has_value());
  EXPECT_EQ(orderbook.best_bid()->price_cents, 53); // highest YES bid, not 1
  ASSERT_TRUE(orderbook.best_ask().has_value());
  EXPECT_EQ(orderbook.best_ask()->price_cents, 56); // 100 - highest NO bid (44)
  EXPECT_DOUBLE_EQ(orderbook.mid_price_cents(), 54.5);
}

TEST(LocalOrderbookTest, DeltaMaintainsOrderAfterAscendingSnapshot) {
  // After the sorted snapshot, a delta that adds a new best bid must land at
  // the front (the descending invariant must hold through delta application
  // too).
  kalshi::Orderbook book;
  book.ticker = "KXBTCD";
  book.yes = {{1, 5000}, {46, 100}, {53, 200}};
  book.no = {{1, 5000}, {40, 100}, {44, 300}};
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(book);

  orderbook.apply_delta(kalshi::Side::Yes, 55, 75); // new top-of-book YES bid

  ASSERT_TRUE(orderbook.best_bid().has_value());
  EXPECT_EQ(orderbook.best_bid()->price_cents, 55);
}

TEST(LocalOrderbookTest, BestBidIsHighestYesPrice) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());
  auto bid = orderbook.best_bid();
  ASSERT_TRUE(bid.has_value());
  const kalshi::Level &bid_level =
      *bid; // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(bid_level.price_cents, kYesBid);
  EXPECT_EQ(bid_level.quantity, kQtyLarge);
}

TEST(LocalOrderbookTest, BestAskIsComplementOfHighestNoPrice) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());
  auto ask = orderbook.best_ask();
  ASSERT_TRUE(ask.has_value());
  const kalshi::Level &ask_level =
      *ask; // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(ask_level.price_cents, kYesAsk);
  EXPECT_EQ(ask_level.quantity, kQtyLarge);
}

TEST(LocalOrderbookTest, MidPriceIsAverageOfBidAndAsk) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());
  EXPECT_DOUBLE_EQ(orderbook.mid_price_cents(),
                   static_cast<double>(kExpectedMid));
}

TEST(LocalOrderbookTest, SpreadIsAskMinusBid) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());
  EXPECT_EQ(orderbook.spread_cents(), kExpectedSpread);
}

// ---- apply_delta ----

TEST(LocalOrderbookTest, ApplyDeltaAddsNewYesLevel) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kNewPrice, kNewQty);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kThreeLevels);
  // kNewPrice = 50 < kYesBid2 = 51, so it goes at the end (sorted descending)
  EXPECT_EQ(yes.back().price_cents, kNewPrice);
  EXPECT_EQ(yes.back().quantity, kNewQty);
}

TEST(LocalOrderbookTest, ApplyDeltaUpdatesExistingYesLevel) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kQtyUpdated);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kTwoLevels);
  EXPECT_EQ(yes[0].price_cents, kYesBid);
  EXPECT_EQ(yes[0].quantity, kQtyUpdated);
}

TEST(LocalOrderbookTest, ApplyDeltaRemovesYesLevelWhenQuantityZero) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, 0);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kOneLevel);
  EXPECT_EQ(yes[0].price_cents, kYesBid2);
}

TEST(LocalOrderbookTest, ApplyDeltaIgnoresRemoveForNonExistentLevel) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kNewPrice,
                        0); // kNewPrice not in book

  EXPECT_EQ(orderbook.state().yes.size(), kTwoLevels); // unchanged
}

TEST(LocalOrderbookTest, ApplyDeltaAddsNoLevel) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  constexpr int kNewNoPrice = 42;
  orderbook.apply_delta(kalshi::Side::No, kNewNoPrice, kNewQty);

  const auto &no_levels = orderbook.state().no;
  ASSERT_EQ(no_levels.size(), kThreeLevels);
  EXPECT_EQ(no_levels.back().price_cents, kNewNoPrice);
}

TEST(LocalOrderbookTest, ApplyDeltaMaintainsSortedDescendingOrder) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  // Insert a price higher than the current best bid
  constexpr int kHigherPrice = 60;
  orderbook.apply_delta(kalshi::Side::Yes, kHigherPrice, kQtySmall);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kThreeLevels);
  EXPECT_EQ(yes[0].price_cents, kHigherPrice); // new best bid
  EXPECT_EQ(yes[1].price_cents, kYesBid);
  EXPECT_EQ(yes[2].price_cents, kYesBid2);
}

TEST(LocalOrderbookTest, BestBidUpdatesAfterDeltaRemovesTopLevel) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, 0); // remove top bid

  auto bid = orderbook.best_bid();
  ASSERT_TRUE(bid.has_value());
  const kalshi::Level &bid_level =
      *bid; // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(bid_level.price_cents, kYesBid2);
}

TEST(LocalOrderbookTest, EmptyYesBookAfterRemovingAllLevels) {
  kalshi::LocalOrderbook orderbook;
  kalshi::Orderbook snap;
  snap.ticker = "KXBTCD";
  snap.yes = {{kYesBid, kQtySmall}};
  orderbook.apply_snapshot(snap);

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, 0);

  EXPECT_FALSE(orderbook.best_bid().has_value());
}
