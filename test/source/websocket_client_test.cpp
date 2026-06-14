#include "fake_websocket.hpp"
#include "websocket_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
#include <string>
#include <vector>

// ---- Test constants ----

namespace {

constexpr int kYesBidPrice = 52;
constexpr int kNoBidPrice = 48;
constexpr int kYesBidQty = 200;
constexpr int kNoBidQty = 150;
constexpr int kDeltaQty = 75;
constexpr int kFillPrice = 52;
constexpr int kFillCount = 5;
constexpr int kSecondLevelPrice = 51;
constexpr int kSecondLevelQty = 100;
constexpr std::size_t kOneLevel = 1U;
constexpr std::size_t kTwoLevels = 2U;
constexpr int kNoReconnect = 0;
constexpr int kOneReconnect = 1;
constexpr int kTwoConnects = 2;

const std::string kTestTicker = "KXBTCD";
const std::string kWsUrl = "wss://trading-api.kalshi.com/trade-api/ws/v2";

// RSA key generated once per test suite (expensive operation).
std::string
    kPemPrivateKey; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
const std::string kApiKey = "test-key-id";

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

kalshi::WebSocketClient
make_client(std::unique_ptr<kalshi::FakeWebSocket> fake_ws,
            int max_reconnects = kNoReconnect) {
  kalshi::Auth auth{kApiKey, kPemPrivateKey};
  return kalshi::WebSocketClient{auth, std::move(fake_ws), kWsUrl,
                                 max_reconnects, std::chrono::milliseconds{0}};
}

// JSON helpers for building test messages.
// These helpers have adjacent same-type parameters by design: callers use
// named constants, so the swappability risk is accepted.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)

std::string snapshot_message(const std::string &ticker, int yes_price,
                             int yes_qty, int no_price, int no_qty) {
  nlohmann::json msg;
  msg["type"] = "orderbook_snapshot";
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["yes"] = {{yes_price, yes_qty}};
  msg["msg"]["no"] = {{no_price, no_qty}};
  return msg.dump();
}

std::string delta_message(const std::string &ticker, const std::string &side,
                          int price, int qty) {
  nlohmann::json msg;
  msg["type"] = "orderbook_delta";
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["side"] = side;
  msg["msg"]["price"] = price;
  msg["msg"]["delta"] = qty;
  return msg.dump();
}

std::string fill_message(const std::string &order_id, const std::string &ticker,
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

// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace

// ---- Test fixture ----

class WebSocketClientTest : public ::testing::Test {
public:
  static void SetUpTestSuite() { kPemPrivateKey = generate_rsa_pem(); }
};

// ---- Subscribe ----

TEST_F(WebSocketClientTest, SubscribeSendsJsonWithCorrectCmd) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.subscribe(kTestTicker);
  client.run();

  ASSERT_EQ(ws_raw->sent_messages().size(), kOneLevel);
  const auto sub = nlohmann::json::parse(ws_raw->sent_messages()[0]);
  EXPECT_EQ(sub["cmd"], "subscribe");
}

TEST_F(WebSocketClientTest, SubscribeSendsCorrectTicker) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.subscribe(kTestTicker);
  client.run();

  ASSERT_EQ(ws_raw->sent_messages().size(), kOneLevel);
  const auto sub = nlohmann::json::parse(ws_raw->sent_messages()[0]);
  const auto &tickers = sub["params"]["market_tickers"];
  ASSERT_EQ(tickers.size(), kOneLevel);
  EXPECT_EQ(tickers[0], kTestTicker);
}

TEST_F(WebSocketClientTest, SubscribeSendsOrderbookDeltaChannel) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.subscribe(kTestTicker);
  client.run();

  ASSERT_EQ(ws_raw->sent_messages().size(), kOneLevel);
  const auto sub = nlohmann::json::parse(ws_raw->sent_messages()[0]);
  const auto &channels = sub["params"]["channels"];
  ASSERT_FALSE(channels.empty());
  EXPECT_EQ(channels[0], "orderbook_delta");
}

TEST_F(WebSocketClientTest, TwoSubscribesSendTwoMessages) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.subscribe("KXBTCD");
  client.subscribe("KXETHU");
  client.run();

  EXPECT_EQ(ws_raw->sent_messages().size(), kTwoLevels);
}

TEST_F(WebSocketClientTest, ConnectUsesConfiguredUrl) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.run();

  EXPECT_EQ(ws_raw->connected_url(), kWsUrl);
}

TEST_F(WebSocketClientTest, ConnectIncludesAuthHeaders) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.run();

  const auto &headers = ws_raw->connected_headers();
  EXPECT_TRUE(headers.contains("Kalshi-Access-Key"));
  EXPECT_TRUE(headers.contains("Kalshi-Access-Timestamp"));
  EXPECT_TRUE(headers.contains("Kalshi-Access-Signature"));
  EXPECT_EQ(headers.at("Kalshi-Access-Key"), kApiKey);
}

// ---- Snapshot parsing ----

TEST_F(WebSocketClientTest, SnapshotCallbackFired) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(snapshot_message(kTestTicker, kYesBidPrice,
                                           kYesBidQty, kNoBidPrice, kNoBidQty));
  auto client = make_client(std::move(fake_ws));

  bool called = false;
  client.on_orderbook_snapshot(
      [&](const kalshi::Orderbook & /*book*/) { called = true; });
  client.run();

  EXPECT_TRUE(called);
}

TEST_F(WebSocketClientTest, SnapshotTickerParsedCorrectly) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(snapshot_message(kTestTicker, kYesBidPrice,
                                           kYesBidQty, kNoBidPrice, kNoBidQty));
  auto client = make_client(std::move(fake_ws));

  std::string recv_ticker;
  client.on_orderbook_snapshot(
      [&](const kalshi::Orderbook &book) { recv_ticker = book.ticker; });
  client.run();

  EXPECT_EQ(recv_ticker, kTestTicker);
}

TEST_F(WebSocketClientTest, SnapshotYesLevelParsedCorrectly) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(snapshot_message(kTestTicker, kYesBidPrice,
                                           kYesBidQty, kNoBidPrice, kNoBidQty));
  auto client = make_client(std::move(fake_ws));

  kalshi::Orderbook received;
  client.on_orderbook_snapshot(
      [&](const kalshi::Orderbook &book) { received = book; });
  client.run();

  ASSERT_EQ(received.yes.size(), kOneLevel);
  EXPECT_EQ(received.yes[0].price_cents, kYesBidPrice);
  EXPECT_EQ(received.yes[0].quantity, kYesBidQty);
}

TEST_F(WebSocketClientTest, SnapshotNoLevelParsedCorrectly) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(snapshot_message(kTestTicker, kYesBidPrice,
                                           kYesBidQty, kNoBidPrice, kNoBidQty));
  auto client = make_client(std::move(fake_ws));

  kalshi::Orderbook received;
  client.on_orderbook_snapshot(
      [&](const kalshi::Orderbook &book) { received = book; });
  client.run();

  ASSERT_EQ(received.no.size(), kOneLevel);
  EXPECT_EQ(received.no[0].price_cents, kNoBidPrice);
  EXPECT_EQ(received.no[0].quantity, kNoBidQty);
}

TEST_F(WebSocketClientTest, SnapshotWithMultipleLevelsPreservesAll) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  nlohmann::json msg;
  msg["type"] = "orderbook_snapshot";
  msg["msg"]["market_ticker"] = kTestTicker;
  msg["msg"]["yes"] = {{kYesBidPrice, kYesBidQty},
                       {kSecondLevelPrice, kSecondLevelQty}};
  msg["msg"]["no"] = nlohmann::json::array();
  ws_raw->enqueue_message(msg.dump());

  auto client = make_client(std::move(fake_ws));

  kalshi::Orderbook received;
  client.on_orderbook_snapshot(
      [&](const kalshi::Orderbook &book) { received = book; });
  client.run();

  ASSERT_EQ(received.yes.size(), kTwoLevels);
  EXPECT_EQ(received.yes[0].price_cents, kYesBidPrice);
  EXPECT_EQ(received.yes[1].price_cents, kSecondLevelPrice);
}

// ---- Delta parsing ----

TEST_F(WebSocketClientTest, DeltaCallbackFiredWithParsedFields) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(
      delta_message(kTestTicker, "yes", kYesBidPrice, kDeltaQty));

  auto client = make_client(std::move(fake_ws));

  std::string recv_ticker;
  kalshi::Side recv_side{kalshi::Side::No};
  int recv_price = 0;
  int recv_qty = 0;
  client.on_orderbook_delta(
      [&](const std::string &ticker, kalshi::Side side,
          int price, // NOLINT(bugprone-easily-swappable-parameters)
          int qty) {
        recv_ticker = ticker;
        recv_side = side;
        recv_price = price;
        recv_qty = qty;
      });

  client.run();

  EXPECT_EQ(recv_ticker, kTestTicker);
  EXPECT_EQ(recv_side, kalshi::Side::Yes);
  EXPECT_EQ(recv_price, kYesBidPrice);
  EXPECT_EQ(recv_qty, kDeltaQty);
}

TEST_F(WebSocketClientTest, DeltaSideNoIsParsedCorrectly) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(
      delta_message(kTestTicker, "no", kNoBidPrice, kDeltaQty));

  auto client = make_client(std::move(fake_ws));

  kalshi::Side recv_side{kalshi::Side::Yes};
  client.on_orderbook_delta([&](const std::string & /*ticker*/,
                                kalshi::Side side, int /*price*/,
                                int /*qty*/) { recv_side = side; });

  client.run();

  EXPECT_EQ(recv_side, kalshi::Side::No);
}

// ---- Fill parsing ----

TEST_F(WebSocketClientTest, FillCallbackFired) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(
      fill_message("order-abc", kTestTicker, "yes", kFillPrice, kFillCount));

  auto client = make_client(std::move(fake_ws));

  bool called = false;
  client.on_fill([&](const kalshi::Fill & /*fill*/) { called = true; });
  client.run();

  EXPECT_TRUE(called);
}

TEST_F(WebSocketClientTest, FillFieldsParsedCorrectly) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(
      fill_message("order-abc", kTestTicker, "yes", kFillPrice, kFillCount));

  auto client = make_client(std::move(fake_ws));

  kalshi::Fill received;
  client.on_fill([&](const kalshi::Fill &fill) { received = fill; });
  client.run();

  EXPECT_EQ(received.order_id, "order-abc");
  EXPECT_EQ(received.market_ticker, kTestTicker);
  EXPECT_EQ(received.side, kalshi::Side::Yes);
  EXPECT_EQ(received.price_cents, kFillPrice);
  EXPECT_EQ(received.quantity, kFillCount);
}

TEST_F(WebSocketClientTest, FillSideNoIsParsedCorrectly) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(
      fill_message("order-xyz", kTestTicker, "no", kNoBidPrice, kFillCount));

  auto client = make_client(std::move(fake_ws));

  kalshi::Side recv_side{kalshi::Side::Yes};
  client.on_fill([&](const kalshi::Fill &fill) { recv_side = fill.side; });
  client.run();

  EXPECT_EQ(recv_side, kalshi::Side::No);
}

// ---- Unknown/malformed messages ----

TEST_F(WebSocketClientTest, UnknownMessageTypeIsIgnored) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  nlohmann::json msg;
  msg["type"] = "trade";
  msg["msg"]["ticker"] = kTestTicker;
  ws_raw->enqueue_message(msg.dump());

  auto client = make_client(std::move(fake_ws));

  bool any_called = false;
  client.on_orderbook_snapshot(
      [&](const kalshi::Orderbook &) { any_called = true; });
  client.on_orderbook_delta(
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [&](const std::string &, kalshi::Side, int, int) { any_called = true; });
  client.on_fill([&](const kalshi::Fill &) { any_called = true; });

  client.run();

  EXPECT_FALSE(any_called);
}

TEST_F(WebSocketClientTest, NoCallbacksRegisteredDoesNotCrash) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(snapshot_message(kTestTicker, kYesBidPrice,
                                           kYesBidQty, kNoBidPrice, kNoBidQty));
  ws_raw->enqueue_message(
      delta_message(kTestTicker, "yes", kYesBidPrice, kDeltaQty));
  ws_raw->enqueue_message(
      fill_message("order-abc", kTestTicker, "yes", kFillPrice, kFillCount));

  auto client = make_client(std::move(fake_ws));
  EXPECT_NO_THROW(client.run());
}

// ---- Reconnect ----

TEST_F(WebSocketClientTest, ReconnectsOnDisconnect) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->trigger_disconnect();

  auto client = make_client(std::move(fake_ws), kOneReconnect);
  client.subscribe(kTestTicker);
  client.run();

  EXPECT_EQ(ws_raw->connect_count(), kTwoConnects);
}

TEST_F(WebSocketClientTest, ResubscribesAfterReconnect) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->trigger_disconnect();

  auto client = make_client(std::move(fake_ws), kOneReconnect);
  client.subscribe(kTestTicker);
  client.run();

  // One subscribe per connect: initial connect + one reconnect = 2.
  EXPECT_EQ(ws_raw->sent_messages().size(), kTwoLevels);
}
