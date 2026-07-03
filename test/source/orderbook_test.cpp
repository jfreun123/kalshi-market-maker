#include "orderbook.hpp"
#include "quantity.hpp"

#include <gtest/gtest.h>

namespace {

using kalshi::Quantity;

constexpr int kYesBid = 52;
constexpr int kYesBid2 = 51;
constexpr int kNoBid = 44;
constexpr int kNoBid2 = 43;
constexpr int kYesAsk = 56;
constexpr int kExpectedMid = 54;
constexpr int kExpectedSpread = 4;
constexpr Quantity kQtyLarge = Quantity::from_contracts(200);
constexpr Quantity kQtySmall = Quantity::from_contracts(100);
constexpr int kNewPrice = 50;
constexpr Quantity kNewQty = Quantity::from_contracts(300);

constexpr Quantity kPositiveDelta = Quantity::from_contracts(75);
constexpr Quantity kNegativeDelta = Quantity::from_contracts(-50);
constexpr Quantity kQtyAfterIncrease = kQtyLarge + kPositiveDelta;
constexpr Quantity kQtyAfterDecrease = kQtyLarge + kNegativeDelta;
constexpr Quantity kRemoveDelta = -kQtyLarge;
constexpr Quantity kOverRemoveDelta = -(kQtyLarge + kQtySmall);
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

TEST(LocalOrderbookTest, EmptyBookMicroPriceReturnsZero) {
  kalshi::LocalOrderbook orderbook;
  EXPECT_DOUBLE_EQ(orderbook.micro_price_cents(), 0.0);
}

TEST(LocalOrderbookTest, BalancedBookMicroPriceEqualsMid) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook()); // best bid/ask both size 200
  EXPECT_DOUBLE_EQ(orderbook.micro_price_cents(), orderbook.mid_price_cents());
}

TEST(LocalOrderbookTest, HeavyBidLeansMicroPriceTowardAsk) {
  kalshi::Orderbook book;
  book.ticker = "KXBTCD";
  book.yes = {{kYesBid, kQtyLarge}}; // best bid 52 x200
  book.no = {{kNoBid, kQtySmall}};   // best ask 56 x100
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(book);

  const double micro = orderbook.micro_price_cents();
  EXPECT_GT(micro, orderbook.mid_price_cents());  // leans up toward the ask
  EXPECT_LT(micro, static_cast<double>(kYesAsk)); // never past the ask
}

TEST(LocalOrderbookTest, HeavyAskLeansMicroPriceTowardBid) {
  kalshi::Orderbook book;
  book.ticker = "KXBTCD";
  book.yes = {{kYesBid, kQtySmall}}; // best bid 52 x100
  book.no = {{kNoBid, kQtyLarge}};   // best ask 56 x200
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(book);

  const double micro = orderbook.micro_price_cents();
  EXPECT_LT(micro, orderbook.mid_price_cents());  // leans down toward the bid
  EXPECT_GT(micro, static_cast<double>(kYesBid)); // never past the bid
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
  book.yes = {{1, Quantity::from_contracts(5000)},
              {46, Quantity::from_contracts(100)},
              {53, Quantity::from_contracts(200)}};
  book.no = {{1, Quantity::from_contracts(5000)},
             {40, Quantity::from_contracts(100)},
             {44, Quantity::from_contracts(300)}};

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
  book.yes = {{1, Quantity::from_contracts(5000)},
              {46, Quantity::from_contracts(100)},
              {53, Quantity::from_contracts(200)}};
  book.no = {{1, Quantity::from_contracts(5000)},
             {40, Quantity::from_contracts(100)},
             {44, Quantity::from_contracts(300)}};
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(book);

  orderbook.apply_delta(kalshi::Side::Yes, 55, Quantity::from_contracts(75));

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

TEST(LocalOrderbookTest, ApplyDeltaIncrementsExistingYesLevel) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kPositiveDelta);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kTwoLevels);
  EXPECT_EQ(yes[0].price_cents, kYesBid);
  EXPECT_EQ(yes[0].quantity, kQtyAfterIncrease);
}

TEST(LocalOrderbookTest, ApplyDeltaDecrementsExistingYesLevel) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kNegativeDelta);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kTwoLevels);
  EXPECT_EQ(yes[0].price_cents, kYesBid);
  EXPECT_EQ(yes[0].quantity, kQtyAfterDecrease);
}

TEST(LocalOrderbookTest, ApplyDeltaAccumulatesAcrossMultipleUpdates) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kPositiveDelta);
  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kNegativeDelta);
  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kNegativeDelta);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kTwoLevels);
  EXPECT_EQ(yes[0].quantity,
            kQtyLarge + kPositiveDelta + kNegativeDelta + kNegativeDelta);
}

TEST(LocalOrderbookTest, ApplyDeltaCarriesFractionalSizeInsteadOfDropping) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid,
                        Quantity::from_fp_string("-0.16"));

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kTwoLevels);
  EXPECT_EQ(yes[0].quantity, kQtyLarge - Quantity::from_fp_string("0.16"));
  EXPECT_EQ(yes[0].quantity, Quantity::from_fp_string("199.84"));
}

TEST(LocalOrderbookTest, ApplyDeltaRemovesYesLevelWhenSizeReachesZero) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kRemoveDelta);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kOneLevel);
  EXPECT_EQ(yes[0].price_cents, kYesBid2);
}

TEST(LocalOrderbookTest, ApplyDeltaRemovesYesLevelWhenSizeGoesNegative) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kOverRemoveDelta);

  const auto &yes = orderbook.state().yes;
  ASSERT_EQ(yes.size(), kOneLevel);
  EXPECT_EQ(yes[0].price_cents, kYesBid2);
}

TEST(LocalOrderbookTest, ApplyDeltaIgnoresShrinkForNonExistentLevel) {
  kalshi::LocalOrderbook orderbook;
  orderbook.apply_snapshot(make_orderbook());

  orderbook.apply_delta(kalshi::Side::Yes, kNewPrice, kNegativeDelta);

  EXPECT_EQ(orderbook.state().yes.size(), kTwoLevels);
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

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, kRemoveDelta);

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

  orderbook.apply_delta(kalshi::Side::Yes, kYesBid, -kQtySmall);

  EXPECT_FALSE(orderbook.best_bid().has_value());
}
