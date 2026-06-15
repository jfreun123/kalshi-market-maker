#include "fake_transport.hpp"
#include "fake_websocket.hpp"
#include "order_manager.hpp"
#include "orderbook.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"
#include "websocket_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ---- Test constants ----

namespace {

// Orderbook: YES bid=51, NO bid=47 → YES ask=53, mid=(51+53)/2=52
constexpr int kYesBid = 51;
constexpr int kNoBid = 47;
constexpr int kObQty = 100;

// Expected quotes at mid=52, default config (spread=4, pos=0):
// fv≈52, half_spread=2, skew=0 → bid=50, ask=54, NO ask=46
constexpr int kExpectedBid = 50;
constexpr int kExpectedNoAsk = 46;
constexpr int kDefaultQuoteSize = kalshi::QuoterConfig::kDefaultQuoteSize;

// Delta below best bid — does not change BBO or mid.
constexpr int kSubBboDeltaPrice = 50;
constexpr int kSubBboDeltaQty = 100;

constexpr int kFillQty = 20;
constexpr int kFillPrice = 50;
constexpr int kHttpOk = 200;

const std::string kTicker = "KXBTCD";
const std::string kOrderId1 = "order-001";
const std::string kOrderId2 = "order-002";
const std::string kFillOrderId = "fill-order-001";
const std::string kApiKey = "test-key-id";
const std::string kBaseUrl = "https://trading-api.kalshi.com/trade-api/v2";
const std::string kWsUrl = "wss://trading-api.kalshi.com/trade-api/ws/v2";

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::string kPemPrivateKey;

std::string generate_rsa_pem() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  EVP_PKEY *pkey = EVP_RSA_gen(2048U);
  if (pkey == nullptr) {
    return "";
  }
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  char *pem_data = nullptr;
  long pem_len = BIO_get_mem_data(bio, &pem_data); // NOLINT(runtime/int)
  std::string pem(pem_data, static_cast<std::size_t>(pem_len));
  BIO_free(bio);
  EVP_PKEY_free(pkey);
  return pem;
}

// ---- WebSocket message builders ----

// NOLINTBEGIN(bugprone-easily-swappable-parameters)

std::string snapshot_msg(const std::string &ticker, int yes_price, int yes_qty,
                         int no_price, int no_qty) {
  nlohmann::json msg;
  msg["type"] = "orderbook_snapshot";
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["yes"] = {{yes_price, yes_qty}};
  msg["msg"]["no"] = {{no_price, no_qty}};
  return msg.dump();
}

std::string delta_msg(const std::string &ticker, const std::string &side,
                      int price, int qty) {
  nlohmann::json msg;
  msg["type"] = "orderbook_delta";
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["side"] = side;
  msg["msg"]["price"] = price;
  msg["msg"]["delta"] = qty;
  return msg.dump();
}

std::string fill_msg(const std::string &order_id, const std::string &ticker,
                     const std::string &side, int price, int count) {
  nlohmann::json msg;
  msg["type"] = "fill";
  msg["msg"]["order_id"] = order_id;
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["side"] = side;
  msg["msg"]["yes_price"] = price;
  msg["msg"]["count"] = count;
  msg["msg"]["created_time"] = "2025-01-01T00:00:00Z";
  return msg.dump();
}

std::string order_json(const std::string &order_id, const std::string &ticker,
                       const std::string &side, int price_cents, int qty) {
  const std::string price_field =
      (side == "yes") ? "\"yes_price\"" : "\"no_price\"";
  return R"({"order":{"order_id":")" + order_id + R"(","ticker":")" + ticker +
         R"(","side":")" + side + R"(",)" + price_field + R"(:)" +
         std::to_string(price_cents) + R"(,"count":)" + std::to_string(qty) +
         R"(,"filled_count":0,"status":"resting","type":"limit",)"
         R"("created_time":"2025-01-01T00:00:00Z"}})";
}

// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace

// ---- Fixture ----

class MainLoopTest : public ::testing::Test {
public:
  static void SetUpTestSuite() { kPemPrivateKey = generate_rsa_pem(); }
};

// ---- Tests ----

// Snapshot seeds the orderbook; the next delta triggers quoter.update().
// Verifies the full pipeline: WS message → orderbook update → REST POST.
TEST_F(MainLoopTest, SnapshotPlusDeltaPlacesQuotes) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue({kHttpOk, order_json(kOrderId1, kTicker, "yes",
                                         kExpectedBid, kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, order_json(kOrderId2, kTicker, "no",
                                         kExpectedNoAsk, kDefaultQuoteSize)});

  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_ptr = fake_ws.get();

  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::WebSocketClient ws_client{
      kalshi::Auth{kApiKey, kPemPrivateKey}, std::move(fake_ws), kWsUrl,
      /*max_reconnects=*/0, std::chrono::milliseconds{0}};

  std::unordered_map<std::string, kalshi::LocalOrderbook> ob_map;
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  const std::vector<std::string> tickers{kTicker};

  ws_client.on_orderbook_snapshot([&ob_map](const kalshi::Orderbook &snap) {
    ob_map[snap.ticker].apply_snapshot(snap);
  });
  ws_client.on_orderbook_delta([&ob_map, &quoter](const std::string &ticker,
                                                  kalshi::Side side, int price,
                                                  int qty) {
    ob_map[ticker].apply_delta(side, price, qty);
    quoter.update(ticker, ob_map[ticker]);
  });
  ws_client.on_fill(
      [&order_mgr, &risk_mgr, &tickers](const kalshi::Fill &fill) {
        order_mgr.record_fill(fill);
        risk_mgr.update(order_mgr, tickers);
      });

  ws_ptr->enqueue_message(
      snapshot_msg(kTicker, kYesBid, kObQty, kNoBid, kObQty));
  ws_ptr->enqueue_message(
      delta_msg(kTicker, "yes", kSubBboDeltaPrice, kSubBboDeltaQty));
  ws_client.run();

  // One bid POST + one ask POST.
  EXPECT_EQ(transport.recorded_requests().size(), 2U);
}

// A delta arriving before any snapshot leaves the orderbook with no NO side,
// so best_ask() returns nullopt and the quoter makes no orders.
TEST_F(MainLoopTest, DeltaWithoutSnapshotPlacesNoOrders) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;

  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_ptr = fake_ws.get();

  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::WebSocketClient ws_client{
      kalshi::Auth{kApiKey, kPemPrivateKey}, std::move(fake_ws), kWsUrl,
      /*max_reconnects=*/0, std::chrono::milliseconds{0}};

  std::unordered_map<std::string, kalshi::LocalOrderbook> ob_map;
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  ws_client.on_orderbook_delta([&ob_map, &quoter](const std::string &ticker,
                                                  kalshi::Side side, int price,
                                                  int qty) {
    ob_map[ticker].apply_delta(side, price, qty);
    quoter.update(ticker, ob_map[ticker]);
  });

  // Delta adds a YES level but no NO levels exist → no best_ask → no orders.
  ws_ptr->enqueue_message(delta_msg(kTicker, "yes", kYesBid, kObQty));
  ws_client.run();

  EXPECT_TRUE(transport.recorded_requests().empty());
}

// Fill event propagates through the on_fill callback into OrderManager.
// Verifies the fill wiring without needing to place or inspect orders.
TEST_F(MainLoopTest, FillEventUpdatesNetPosition) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_ptr = fake_ws.get();

  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::WebSocketClient ws_client{
      kalshi::Auth{kApiKey, kPemPrivateKey}, std::move(fake_ws), kWsUrl,
      /*max_reconnects=*/0, std::chrono::milliseconds{0}};

  std::unordered_map<std::string, kalshi::LocalOrderbook> ob_map;
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  const std::vector<std::string> tickers{kTicker};

  ws_client.on_fill(
      [&order_mgr, &risk_mgr, &tickers](const kalshi::Fill &fill) {
        order_mgr.record_fill(fill);
        risk_mgr.update(order_mgr, tickers);
      });

  ws_ptr->enqueue_message(
      fill_msg(kFillOrderId, kTicker, "yes", kFillPrice, kFillQty));
  ws_client.run();

  EXPECT_EQ(order_mgr.net_position(kTicker), kFillQty);
}

// When RiskManager is halted, check_order() returns false and no orders are
// placed even when the orderbook has a valid BBO.
TEST_F(MainLoopTest, NoOrdersPlacedWhenRiskHalted) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;

  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_ptr = fake_ws.get();

  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::WebSocketClient ws_client{
      kalshi::Auth{kApiKey, kPemPrivateKey}, std::move(fake_ws), kWsUrl,
      /*max_reconnects=*/0, std::chrono::milliseconds{0}};

  std::unordered_map<std::string, kalshi::LocalOrderbook> ob_map;
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  risk_mgr.halt();
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  ws_client.on_orderbook_snapshot([&ob_map](const kalshi::Orderbook &snap) {
    ob_map[snap.ticker].apply_snapshot(snap);
  });
  ws_client.on_orderbook_delta([&ob_map, &quoter](const std::string &ticker,
                                                  kalshi::Side side, int price,
                                                  int qty) {
    ob_map[ticker].apply_delta(side, price, qty);
    quoter.update(ticker, ob_map[ticker]);
  });

  ws_ptr->enqueue_message(
      snapshot_msg(kTicker, kYesBid, kObQty, kNoBid, kObQty));
  ws_ptr->enqueue_message(
      delta_msg(kTicker, "yes", kSubBboDeltaPrice, kSubBboDeltaQty));
  ws_client.run();

  EXPECT_TRUE(transport.recorded_requests().empty());
}
