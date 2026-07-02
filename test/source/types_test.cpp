#include "types.hpp"

#include <chrono>
#include <gtest/gtest.h>

using namespace kalshi;

// ---- Side ----

TEST(SideTest, ValuesAreDistinct) { EXPECT_NE(Side::Yes, Side::No); }

// ---- OrderStatus ----

TEST(OrderStatusTest, ValuesAreDistinct) {
  EXPECT_NE(OrderStatus::Open, OrderStatus::Filled);
  EXPECT_NE(OrderStatus::PartiallyFilled, OrderStatus::Cancelled);
}

// ---- OrderType ----

TEST(OrderTypeTest, ValuesAreDistinct) {
  EXPECT_NE(OrderType::Limit, OrderType::Market);
}

// ---- Level ----

TEST(LevelTest, FieldAccess) {
  constexpr int kPrice = 52;
  const Quantity kQty = Quantity::from_contracts(100);
  Level level{kPrice, kQty};
  EXPECT_EQ(level.price_cents, kPrice);
  EXPECT_EQ(level.quantity, kQty);
}

TEST(LevelTest, Equality) {
  constexpr int kPriceA = 52;
  constexpr int kPriceB = 53;
  const Quantity kQty = Quantity::from_contracts(100);
  Level lhs{kPriceA, kQty};
  Level rhs{kPriceA, kQty};
  Level other{kPriceB, kQty};
  EXPECT_EQ(lhs, rhs);
  EXPECT_NE(lhs, other);
}

// ---- Orderbook ----

TEST(OrderbookTest, DefaultConstruction) {
  Orderbook book;
  EXPECT_TRUE(book.ticker.empty());
  EXPECT_TRUE(book.yes.empty());
  EXPECT_TRUE(book.no.empty());
}

TEST(OrderbookTest, PushLevels) {
  constexpr int kYesPrice = 55;
  const Quantity kYesQty = Quantity::from_contracts(200);
  constexpr int kNoPrice = 42;
  const Quantity kNoQty = Quantity::from_contracts(50);
  Orderbook book;
  book.ticker = "KXBTCD-25DEC31-T50000";
  book.yes.push_back({kYesPrice, kYesQty});
  book.no.push_back({kNoPrice, kNoQty});
  EXPECT_EQ(book.ticker, "KXBTCD-25DEC31-T50000");
  ASSERT_EQ(book.yes.size(), 1U);
  ASSERT_EQ(book.no.size(), 1U);
  EXPECT_EQ(book.yes[0].price_cents, kYesPrice);
  EXPECT_EQ(book.no[0].price_cents, kNoPrice);
}

// ---- Order ----

TEST(OrderTest, FieldAccess) {
  constexpr int kPrice = 52;
  const Quantity kQty = Quantity::from_contracts(10);
  Order order;
  order.id = "order-abc";
  order.market_ticker = "KXBTCD-25DEC31-T50000";
  order.side = Side::Yes;
  order.price_cents = kPrice;
  order.quantity = kQty;
  order.filled_quantity = Quantity{};
  order.status = OrderStatus::Open;
  order.type = OrderType::Limit;

  EXPECT_EQ(order.id, "order-abc");
  EXPECT_EQ(order.side, Side::Yes);
  EXPECT_EQ(order.price_cents, kPrice);
  EXPECT_EQ(order.quantity, kQty);
  EXPECT_EQ(order.filled_quantity, Quantity{});
  EXPECT_EQ(order.status, OrderStatus::Open);
  EXPECT_EQ(order.type, OrderType::Limit);
}

TEST(OrderTest, RemainingQuantity) {
  const Quantity kTotal = Quantity::from_contracts(10);
  const Quantity kFilled = Quantity::from_contracts(3);
  Order order;
  order.quantity = kTotal;
  order.filled_quantity = kFilled;
  EXPECT_EQ(order.remaining_quantity(), kTotal - kFilled);
}

TEST(OrderTest, RemainingQuantityFullyFilled) {
  const Quantity kQty = Quantity::from_contracts(10);
  Order order;
  order.quantity = kQty;
  order.filled_quantity = kQty;
  EXPECT_EQ(order.remaining_quantity(), Quantity{});
}

TEST(OrderTest, IsActiveWhenOpen) {
  Order order;
  order.status = OrderStatus::Open;
  EXPECT_TRUE(order.is_active());
}

TEST(OrderTest, IsActiveWhenPartiallyFilled) {
  Order order;
  order.status = OrderStatus::PartiallyFilled;
  EXPECT_TRUE(order.is_active());
}

TEST(OrderTest, IsInactiveWhenFilled) {
  Order order;
  order.status = OrderStatus::Filled;
  EXPECT_FALSE(order.is_active());
}

TEST(OrderTest, IsInactiveWhenCancelled) {
  Order order;
  order.status = OrderStatus::Cancelled;
  EXPECT_FALSE(order.is_active());
}

// ---- Fill ----

TEST(FillTest, FieldAccess) {
  constexpr int kPrice = 48;
  const Quantity kQty = Quantity::from_contracts(5);
  Fill fill;
  fill.order_id = "order-abc";
  fill.market_ticker = "KXBTCD-25DEC31-T50000";
  fill.side = Side::No;
  fill.price_cents = kPrice;
  fill.quantity = kQty;
  fill.timestamp = std::chrono::system_clock::now();

  EXPECT_EQ(fill.order_id, "order-abc");
  EXPECT_EQ(fill.market_ticker, "KXBTCD-25DEC31-T50000");
  EXPECT_EQ(fill.side, Side::No);
  EXPECT_EQ(fill.price_cents, kPrice);
  EXPECT_EQ(fill.quantity, kQty);
}

// ---- Market ----

TEST(MarketTest, FieldAccess) {
  constexpr int kFeeRateBps = 7;
  Market market;
  market.ticker = "KXBTCD-25DEC31-T50000";
  market.title = "Will BTC close above $50k on Dec 31?";
  market.fee_rate_bps = kFeeRateBps;
  market.close_time = std::chrono::system_clock::now();

  EXPECT_EQ(market.ticker, "KXBTCD-25DEC31-T50000");
  EXPECT_EQ(market.title, "Will BTC close above $50k on Dec 31?");
  EXPECT_EQ(market.fee_rate_bps, kFeeRateBps);
}

// ---- Price helpers ----

TEST(PriceHelpersTest, IsValidPrice) {
  constexpr int kMinPrice = 1;
  constexpr int kMidPrice = 50;
  constexpr int kMaxPrice = 99;
  EXPECT_TRUE(is_valid_price(kMinPrice));
  EXPECT_TRUE(is_valid_price(kMidPrice));
  EXPECT_TRUE(is_valid_price(kMaxPrice));
  EXPECT_FALSE(is_valid_price(0));
  EXPECT_FALSE(is_valid_price(100));
  EXPECT_FALSE(is_valid_price(-1));
  EXPECT_FALSE(is_valid_price(150));
}

TEST(PriceHelpersTest, ComplementPrice) {
  EXPECT_EQ(complement_price(40), 60);
  EXPECT_EQ(complement_price(1), 99);
  EXPECT_EQ(complement_price(99), 1);
  EXPECT_EQ(complement_price(50), 50);
}

TEST(PriceHelpersTest, ComplementIsSymmetric) {
  constexpr int kMaxPrice = 99;
  for (int price = 1; price <= kMaxPrice; ++price) {
    EXPECT_EQ(complement_price(complement_price(price)), price);
  }
}
