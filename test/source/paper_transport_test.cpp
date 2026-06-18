#include "paper_transport.hpp"

#include "types.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(PaperTransportTest, PlaceOrderReturnsSyntheticOrder) {
  kalshi::PaperTransport paper;

  const std::string body = R"({
    "ticker": "TICK-A",
    "action": "buy",
    "side": "yes",
    "type": "limit",
    "count": 10,
    "yes_price": 52
  })";

  const auto response = paper.post("/portfolio/orders", {}, body);

  EXPECT_EQ(response.status_code, 201);
  EXPECT_FALSE(response.body.empty());
  ASSERT_EQ(paper.open_orders().size(), 1U);
  EXPECT_EQ(paper.open_orders().front().market_ticker, "TICK-A");
  EXPECT_EQ(paper.open_orders().front().side, kalshi::Side::Yes);
  EXPECT_EQ(paper.open_orders().front().price_cents, 52);
  EXPECT_EQ(paper.open_orders().front().quantity, 10);
  EXPECT_EQ(paper.open_orders().front().status, kalshi::OrderStatus::Open);
}

TEST(PaperTransportTest, CancelOrderRemovesItFromOpenOrders) {
  kalshi::PaperTransport paper;

  const std::string body =
      R"({"ticker":"T","action":"buy","side":"yes","type":"limit","count":5,"yes_price":48})";
  (void)paper.post("/portfolio/orders", {}, body);

  ASSERT_EQ(paper.open_orders().size(), 1U);
  const std::string order_id = paper.open_orders().front().id;

  const auto response = paper.delete_("/portfolio/orders/" + order_id, {});

  EXPECT_EQ(response.status_code, 200);
  EXPECT_TRUE(paper.open_orders().empty());
}

TEST(PaperTransportTest, GetOpenOrdersReturnsCurrentBook) {
  kalshi::PaperTransport paper;

  const std::string body_a =
      R"({"ticker":"T","action":"buy","side":"yes","type":"limit","count":5,"yes_price":48})";
  const std::string body_b =
      R"({"ticker":"T","action":"buy","side":"no","type":"limit","count":3,"no_price":50})";
  (void)paper.post("/portfolio/orders", {}, body_a);
  (void)paper.post("/portfolio/orders", {}, body_b);

  const auto response = paper.get("/portfolio/orders?status=resting", {});

  EXPECT_EQ(response.status_code, 200);
  ASSERT_EQ(paper.open_orders().size(), 2U);
}

TEST(PaperTransportTest, SimulateFillUpdatesOrderStatus) {
  kalshi::PaperTransport paper;

  const std::string body =
      R"({"ticker":"T","action":"buy","side":"yes","type":"limit","count":10,"yes_price":52})";
  (void)paper.post("/portfolio/orders", {}, body);

  const std::string order_id = paper.open_orders().front().id;

  // Partial fill
  EXPECT_TRUE(paper.simulate_fill(order_id, 4));
  ASSERT_EQ(paper.open_orders().size(), 1U);
  EXPECT_EQ(paper.open_orders().front().filled_quantity, 4);
  EXPECT_EQ(paper.open_orders().front().status,
            kalshi::OrderStatus::PartiallyFilled);
  EXPECT_EQ(paper.fills().size(), 1U);
  EXPECT_EQ(paper.fills().front().quantity, 4);

  // Fill the rest
  EXPECT_TRUE(paper.simulate_fill(order_id, 6));
  EXPECT_TRUE(paper.open_orders().empty()); // removed when fully filled
  EXPECT_EQ(paper.fills().size(), 2U);
}

TEST(PaperTransportTest, SimulateFillReturnsFalseForUnknownOrderId) {
  kalshi::PaperTransport paper;

  EXPECT_FALSE(paper.simulate_fill("nonexistent-id", 1));
}

TEST(PaperTransportTest, SimulateFillClampsToRemainingQuantity) {
  kalshi::PaperTransport paper;

  const std::string body =
      R"({"ticker":"T","action":"buy","side":"yes","type":"limit","count":5,"yes_price":60})";
  (void)paper.post("/portfolio/orders", {}, body);
  const std::string order_id = paper.open_orders().front().id;

  // Request fill of 100, but only 5 remain.
  constexpr int kOversizedFill = 100;
  EXPECT_TRUE(paper.simulate_fill(order_id, kOversizedFill));
  EXPECT_EQ(paper.fills().front().quantity, 5);
  EXPECT_TRUE(paper.open_orders().empty());
}

TEST(PaperTransportTest, MultipleOrdersGetUniqueIds) {
  kalshi::PaperTransport paper;
  const std::string body =
      R"({"ticker":"T","action":"buy","side":"yes","type":"limit","count":1,"yes_price":50})";

  (void)paper.post("/portfolio/orders", {}, body);
  (void)paper.post("/portfolio/orders", {}, body);

  ASSERT_EQ(paper.open_orders().size(), 2U);
  EXPECT_NE(paper.open_orders().at(0).id, paper.open_orders().at(1).id);
}
