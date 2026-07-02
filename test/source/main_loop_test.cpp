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

#include <format>
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

constexpr int kDefaultQuoteSize = kalshi::QuoterConfig::kDefaultQuoteSize;

// Delta below best bid — does not change BBO or mid.
constexpr int kSubBboDeltaPrice = 50;
constexpr int kSubBboDeltaQty = 100;

constexpr int kFillQty = 20;
constexpr int kFillPrice = 50;
constexpr int kHttpOk = 200;
constexpr double kCentsPerDollar = 100.0;
constexpr long long kFillTsMs = 1735689600000LL; // 2025-01-01T00:00:00Z in ms

const std::string kTicker = "KXBTCD";
const std::string kOrderId1 = "order-001";
const std::string kOrderId2 = "order-002";
const std::string kFillOrderId = "fill-order-001";
const std::string kApiKey = "test-key-id";
const std::string kBaseUrl = "https://external-api.kalshi.com/trade-api/v2";
const std::string kWsUrl = "wss://external-api-ws.kalshi.com/trade-api/ws/v2";

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

// nlohmann/json treats {{str, str}} as a key-value object, not a nested array.
// Use json::array() explicitly for [[price, count], ...] structures.
nlohmann::json make_level(int price_cents, int qty) {
  return nlohmann::json::array(
      {std::format("{:.4f}", price_cents / kCentsPerDollar),
       std::format("{:.2f}", static_cast<double>(qty))});
}

std::string snapshot_msg(const std::string &ticker, int yes_price, int yes_qty,
                         int no_price, int no_qty) {
  nlohmann::json msg;
  msg["type"] = "orderbook_snapshot";
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["yes_dollars_fp"] =
      nlohmann::json::array({make_level(yes_price, yes_qty)});
  msg["msg"]["no_dollars_fp"] =
      nlohmann::json::array({make_level(no_price, no_qty)});
  return msg.dump();
}

std::string delta_msg(const std::string &ticker, const std::string &side,
                      int price, int qty) {
  nlohmann::json msg;
  msg["type"] = "orderbook_delta";
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["side"] = side;
  msg["msg"]["price_dollars"] = std::format("{:.4f}", price / kCentsPerDollar);
  msg["msg"]["delta_fp"] = std::format("{:.2f}", static_cast<double>(qty));
  return msg.dump();
}

std::string fill_msg(const std::string &order_id, const std::string &ticker,
                     const std::string &side, int price, int count) {
  nlohmann::json msg;
  msg["type"] = "fill";
  msg["msg"]["order_id"] = order_id;
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["outcome_side"] = side;
  msg["msg"]["yes_price_dollars"] =
      std::format("{:.4f}", price / kCentsPerDollar);
  msg["msg"]["count_fp"] = std::format("{:.2f}", static_cast<double>(count));
  msg["msg"]["ts_ms"] = kFillTsMs;
  msg["msg"]["is_taker"] = false;
  return msg.dump();
}

// Builds the V2 minimal response body that RestClient expects for a placed
// order.
std::string order_json(const std::string &order_id, int qty) {
  return R"({"order_id":")" + order_id +
         R"(","fill_count_fp":"0.00","remaining_count":")" +
         std::to_string(qty) + R"(.00","ts_ms":1718000000000})";
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
  transport.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});

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
                                                  kalshi::Quantity qty) {
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
                                                  kalshi::Quantity qty) {
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

  EXPECT_EQ(order_mgr.net_position(kTicker),
            kalshi::Quantity::from_contracts(kFillQty));
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
                                                  kalshi::Quantity qty) {
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
