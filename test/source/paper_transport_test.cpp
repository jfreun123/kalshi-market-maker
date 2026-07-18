#include "net/paper_transport.hpp"

#include "core/types.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <format>
#include <string>

namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpCreated = 201;
constexpr double kCentsPerDollar = 100.0;

// Builds the V2 create-order body exactly as RestClient::place_order emits it:
// side is "bid"/"ask" in the YES dimension, price/count are fixed-point
// strings. NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::string v2_order_body(const std::string &ticker, kalshi::Side side,
                          int price_cents, int quantity) {
  const int yes_price_cents =
      (side == kalshi::Side::Yes) ? price_cents : (100 - price_cents);
  nlohmann::json body;
  body["ticker"] = ticker;
  body["side"] = (side == kalshi::Side::Yes) ? "bid" : "ask";
  body["price"] = std::format("{:.4f}", yes_price_cents / kCentsPerDollar);
  body["count"] = std::format("{:.2f}", static_cast<double>(quantity));
  body["time_in_force"] = "good_till_canceled";
  return body.dump();
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace

TEST(PaperTransportTest, PlaceOrderReturnsSyntheticOrder) {
  kalshi::PaperTransport paper;

  constexpr int kPrice = 52;
  constexpr int kQty = 10;
  const auto response =
      paper.post("/portfolio/orders", {},
                 v2_order_body("TICK-A", kalshi::Side::Yes, kPrice, kQty));

  EXPECT_EQ(response.status_code, kHttpCreated);
  // Response must carry the V2 fields RestClient::place_order reads back.
  const auto parsed = nlohmann::json::parse(response.body);
  EXPECT_TRUE(parsed.contains("order_id"));
  EXPECT_TRUE(parsed.contains("fill_count"));
  EXPECT_TRUE(parsed.contains("ts_ms"));

  ASSERT_EQ(paper.open_orders().size(), 1U);
  EXPECT_EQ(paper.open_orders().front().market_ticker, "TICK-A");
  EXPECT_EQ(paper.open_orders().front().side, kalshi::Side::Yes);
  EXPECT_EQ(paper.open_orders().front().price_cents, kPrice);
  EXPECT_EQ(paper.open_orders().front().quantity,
            kalshi::Quantity::from_contracts(kQty));
  EXPECT_EQ(paper.open_orders().front().status, kalshi::OrderStatus::Open);
}

TEST(PaperTransportTest, PlaceAskOrderStoresNoSidePrice) {
  kalshi::PaperTransport paper;

  // A "buy NO at 40c" order: RestClient sends side=ask, price=YES 60c.
  constexpr int kNoPrice = 40;
  constexpr int kQty = 5;
  (void)paper.post("/portfolio/orders", {},
                   v2_order_body("T", kalshi::Side::No, kNoPrice, kQty));

  ASSERT_EQ(paper.open_orders().size(), 1U);
  EXPECT_EQ(paper.open_orders().front().side, kalshi::Side::No);
  EXPECT_EQ(paper.open_orders().front().price_cents, kNoPrice);
}

TEST(PaperTransportTest, CancelOrderRemovesItFromOpenOrders) {
  kalshi::PaperTransport paper;

  constexpr int kPrice = 48;
  constexpr int kQty = 5;
  (void)paper.post("/portfolio/orders", {},
                   v2_order_body("T", kalshi::Side::Yes, kPrice, kQty));

  ASSERT_EQ(paper.open_orders().size(), 1U);
  const std::string order_id = paper.open_orders().front().id;

  const auto response = paper.delete_("/portfolio/orders/" + order_id, {});

  EXPECT_EQ(response.status_code, kHttpOk);
  EXPECT_TRUE(paper.open_orders().empty());
}

TEST(PaperTransportTest, GetOpenOrdersReturnsCurrentBook) {
  kalshi::PaperTransport paper;

  constexpr int kYesPrice = 48;
  constexpr int kNoPrice = 50;
  constexpr int kYesQty = 5;
  constexpr int kNoQty = 3;
  (void)paper.post("/portfolio/orders", {},
                   v2_order_body("T", kalshi::Side::Yes, kYesPrice, kYesQty));
  (void)paper.post("/portfolio/orders", {},
                   v2_order_body("T", kalshi::Side::No, kNoPrice, kNoQty));

  const auto response = paper.get("/portfolio/orders?status=resting", {});

  EXPECT_EQ(response.status_code, kHttpOk);
  ASSERT_EQ(paper.open_orders().size(), 2U);
}

TEST(PaperTransportTest, SimulateFillUpdatesOrderStatus) {
  kalshi::PaperTransport paper;

  constexpr int kPrice = 52;
  constexpr int kQty = 10;
  (void)paper.post("/portfolio/orders", {},
                   v2_order_body("T", kalshi::Side::Yes, kPrice, kQty));

  const std::string order_id = paper.open_orders().front().id;

  // Partial fill
  constexpr int kPartialFill = 4;
  EXPECT_TRUE(paper.simulate_fill(order_id, kPartialFill));
  ASSERT_EQ(paper.open_orders().size(), 1U);
  EXPECT_EQ(paper.open_orders().front().filled_quantity,
            kalshi::Quantity::from_contracts(kPartialFill));
  EXPECT_EQ(paper.open_orders().front().status,
            kalshi::OrderStatus::PartiallyFilled);
  EXPECT_EQ(paper.fills().size(), 1U);
  EXPECT_EQ(paper.fills().front().quantity,
            kalshi::Quantity::from_contracts(kPartialFill));

  // Fill the rest
  constexpr int kRemainingFill = 6;
  EXPECT_TRUE(paper.simulate_fill(order_id, kRemainingFill));
  EXPECT_TRUE(paper.open_orders().empty()); // removed when fully filled
  EXPECT_EQ(paper.fills().size(), 2U);
}

TEST(PaperTransportTest, SimulateFillReturnsFalseForUnknownOrderId) {
  kalshi::PaperTransport paper;

  EXPECT_FALSE(paper.simulate_fill("nonexistent-id", 1));
}

TEST(PaperTransportTest, SimulateFillClampsToRemainingQuantity) {
  kalshi::PaperTransport paper;

  constexpr int kPrice = 60;
  constexpr int kQty = 5;
  (void)paper.post("/portfolio/orders", {},
                   v2_order_body("T", kalshi::Side::Yes, kPrice, kQty));
  const std::string order_id = paper.open_orders().front().id;

  // Request fill of 100, but only 5 remain.
  constexpr int kOversizedFill = 100;
  EXPECT_TRUE(paper.simulate_fill(order_id, kOversizedFill));
  EXPECT_EQ(paper.fills().front().quantity,
            kalshi::Quantity::from_contracts(kQty));
  EXPECT_TRUE(paper.open_orders().empty());
}

TEST(PaperTransportTest, MultipleOrdersGetUniqueIds) {
  kalshi::PaperTransport paper;

  const std::string body = v2_order_body("T", kalshi::Side::Yes, 50, 1);
  (void)paper.post("/portfolio/orders", {}, body);
  (void)paper.post("/portfolio/orders", {}, body);

  ASSERT_EQ(paper.open_orders().size(), 2U);
  EXPECT_NE(paper.open_orders().at(0).id, paper.open_orders().at(1).id);
}
