#include "paper_transport.hpp"

#include "auth.hpp"
#include "rest_client.hpp"
#include "types.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

// ---- Helpers ----

// Builds a PaperTransport-backed RestClient using a trivially invalid auth.
// All HTTP calls are intercepted before any real network access, so the auth
// key content is irrelevant.
kalshi::RestClient
make_paper_rest_client(kalshi::PaperTransport *&out_transport) {
  auto transport = std::make_unique<kalshi::PaperTransport>();
  out_transport = transport.get();
  const kalshi::Auth auth{
      "paper-test-key",
      "-----BEGIN RSA PRIVATE KEY-----\n"
      "MIIEowIBAAKCAQEA0Z3VS5JJcds3xHn/ygWep4PAtEsHAqRTGGpAc7pOtRJp\n"
      "-----END RSA PRIVATE KEY-----\n"};

  // RestClient signs requests but PaperTransport ignores headers entirely.
  // Use FakeAuth by exploiting the fact that Auth::sign() only fails at
  // OpenSSL level — to avoid the dependency, construct the client directly.
  // Since we cannot construct Auth with an invalid key, use the RestClient
  // constructor that accepts a transport and a dummy base_url.
  //
  // Actually: Auth constructor validates the PEM at construction time.
  // Use a known RSA-2048 private key generated for testing only.
  (void)auth;
  return kalshi::RestClient{kalshi::Auth{"k", "k"}, std::move(transport),
                            "https://paper.example.com/trade-api/v2"};
}

} // namespace

// ---- place_order via RestClient → PaperTransport ----

TEST(PaperTransportTest, PlaceOrderReturnsSyntheticOrder) {
  kalshi::PaperTransport paper;

  // Place order directly through PaperTransport's post() method.
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
  paper.post("/portfolio/orders", {}, body);

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
  paper.post("/portfolio/orders", {}, body_a);
  paper.post("/portfolio/orders", {}, body_b);

  const auto response = paper.get("/portfolio/orders?status=resting", {});

  EXPECT_EQ(response.status_code, 200);
  ASSERT_EQ(paper.open_orders().size(), 2U);
}

TEST(PaperTransportTest, SimulateFillUpdatesOrderStatus) {
  kalshi::PaperTransport paper;

  const std::string body =
      R"({"ticker":"T","action":"buy","side":"yes","type":"limit","count":10,"yes_price":52})";
  paper.post("/portfolio/orders", {}, body);

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
  paper.post("/portfolio/orders", {}, body);
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

  paper.post("/portfolio/orders", {}, body);
  paper.post("/portfolio/orders", {}, body);

  ASSERT_EQ(paper.open_orders().size(), 2U);
  EXPECT_NE(paper.open_orders().at(0).id, paper.open_orders().at(1).id);
}
