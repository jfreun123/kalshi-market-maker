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
  Level l{52, 100};
  EXPECT_EQ(l.price_cents, 52);
  EXPECT_EQ(l.quantity, 100);
}

TEST(LevelTest, Equality) {
  Level a{52, 100};
  Level b{52, 100};
  Level c{53, 100};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

// ---- Orderbook ----

TEST(OrderbookTest, DefaultConstruction) {
  Orderbook ob;
  EXPECT_TRUE(ob.ticker.empty());
  EXPECT_TRUE(ob.yes.empty());
  EXPECT_TRUE(ob.no.empty());
}

TEST(OrderbookTest, PushLevels) {
  Orderbook ob;
  ob.ticker = "KXBTCD-25DEC31-T50000";
  ob.yes.push_back({55, 200});
  ob.no.push_back({42, 50});
  EXPECT_EQ(ob.ticker, "KXBTCD-25DEC31-T50000");
  ASSERT_EQ(ob.yes.size(), 1u);
  ASSERT_EQ(ob.no.size(), 1u);
  EXPECT_EQ(ob.yes[0].price_cents, 55);
  EXPECT_EQ(ob.no[0].price_cents, 42);
}

// ---- Order ----

TEST(OrderTest, FieldAccess) {
  Order o;
  o.id = "order-abc";
  o.market_ticker = "KXBTCD-25DEC31-T50000";
  o.side = Side::Yes;
  o.price_cents = 52;
  o.quantity = 10;
  o.filled_quantity = 0;
  o.status = OrderStatus::Open;
  o.type = OrderType::Limit;

  EXPECT_EQ(o.id, "order-abc");
  EXPECT_EQ(o.side, Side::Yes);
  EXPECT_EQ(o.price_cents, 52);
  EXPECT_EQ(o.quantity, 10);
  EXPECT_EQ(o.filled_quantity, 0);
  EXPECT_EQ(o.status, OrderStatus::Open);
  EXPECT_EQ(o.type, OrderType::Limit);
}

TEST(OrderTest, RemainingQuantity) {
  Order o;
  o.quantity = 10;
  o.filled_quantity = 3;
  EXPECT_EQ(o.remaining_quantity(), 7);
}

TEST(OrderTest, RemainingQuantityFullyFilled) {
  Order o;
  o.quantity = 10;
  o.filled_quantity = 10;
  EXPECT_EQ(o.remaining_quantity(), 0);
}

TEST(OrderTest, IsActiveWhenOpen) {
  Order o;
  o.status = OrderStatus::Open;
  EXPECT_TRUE(o.is_active());
}

TEST(OrderTest, IsActiveWhenPartiallyFilled) {
  Order o;
  o.status = OrderStatus::PartiallyFilled;
  EXPECT_TRUE(o.is_active());
}

TEST(OrderTest, IsInactiveWhenFilled) {
  Order o;
  o.status = OrderStatus::Filled;
  EXPECT_FALSE(o.is_active());
}

TEST(OrderTest, IsInactiveWhenCancelled) {
  Order o;
  o.status = OrderStatus::Cancelled;
  EXPECT_FALSE(o.is_active());
}

// ---- Fill ----

TEST(FillTest, FieldAccess) {
  Fill f;
  f.order_id = "order-abc";
  f.market_ticker = "KXBTCD-25DEC31-T50000";
  f.side = Side::No;
  f.price_cents = 48;
  f.quantity = 5;
  f.timestamp = std::chrono::system_clock::now();

  EXPECT_EQ(f.order_id, "order-abc");
  EXPECT_EQ(f.market_ticker, "KXBTCD-25DEC31-T50000");
  EXPECT_EQ(f.side, Side::No);
  EXPECT_EQ(f.price_cents, 48);
  EXPECT_EQ(f.quantity, 5);
}

// ---- Market ----

TEST(MarketTest, FieldAccess) {
  Market m;
  m.ticker = "KXBTCD-25DEC31-T50000";
  m.title = "Will BTC close above $50k on Dec 31?";
  m.fee_rate_bps = 7;
  m.close_time = std::chrono::system_clock::now();

  EXPECT_EQ(m.ticker, "KXBTCD-25DEC31-T50000");
  EXPECT_EQ(m.title, "Will BTC close above $50k on Dec 31?");
  EXPECT_EQ(m.fee_rate_bps, 7);
}

// ---- Price helpers ----

TEST(PriceHelpersTest, IsValidPrice) {
  EXPECT_TRUE(is_valid_price(1));
  EXPECT_TRUE(is_valid_price(50));
  EXPECT_TRUE(is_valid_price(99));
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
  for (int p = 1; p <= 99; ++p) {
    EXPECT_EQ(complement_price(complement_price(p)), p);
  }
}
