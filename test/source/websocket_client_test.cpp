#include "fake_websocket.hpp"
#include "websocket_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <format>
#include <memory>
#include <stdexcept>
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
constexpr double kCentsPerDollar = 100.0;
constexpr long long kFillTsMs = 1735689600000LL; // 2025-01-01T00:00:00Z in ms
constexpr std::size_t kOneLevel = 1U;
constexpr std::size_t kTwoLevels = 2U;
constexpr std::size_t kFillPlusOneMarket = 2U;  // fill sub + 1 market sub
constexpr std::size_t kFillPlusTwoMarkets = 3U; // fill sub + 2 market subs
constexpr std::size_t kFourMessages = 4U; // 2 reconnects x (fill + market)
constexpr int kNoReconnect = 0;
constexpr int kOneReconnect = 1;
constexpr int kTwoConnects = 2;

const std::string kTestTicker = "KXBTCD";
const std::string kWsUrl = "wss://external-api-ws.kalshi.com/trade-api/ws/v2";

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

// Format int cents as fixed-point dollar string: 52 -> "0.5200"
std::string cents_to_dollars(int cents) {
  return std::format("{:.4f}", cents / kCentsPerDollar);
}

// Format int count as fixed-point string: 10 -> "10.00"
std::string format_count(int count) {
  return std::format("{:.2f}", static_cast<double>(count));
}

// nlohmann/json treats {{str, str}} as a key-value object, not a nested array.
// Must use json::array() explicitly to build [[price, count], ...] structures.
nlohmann::json make_level(int price_cents, int qty) {
  return nlohmann::json::array(
      {cents_to_dollars(price_cents), format_count(qty)});
}

std::string snapshot_message(const std::string &ticker, int yes_price,
                             int yes_qty, int no_price, int no_qty) {
  nlohmann::json msg;
  msg["type"] = "orderbook_snapshot";
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["yes_dollars_fp"] =
      nlohmann::json::array({make_level(yes_price, yes_qty)});
  msg["msg"]["no_dollars_fp"] =
      nlohmann::json::array({make_level(no_price, no_qty)});
  return msg.dump();
}

std::string delta_message(const std::string &ticker, const std::string &side,
                          int price, int qty) {
  nlohmann::json msg;
  msg["type"] = "orderbook_delta";
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["side"] = side;
  msg["msg"]["price_dollars"] = cents_to_dollars(price);
  msg["msg"]["delta_fp"] = format_count(qty);
  return msg.dump();
}

std::string fill_message(const std::string &order_id, const std::string &ticker,
                         const std::string &side, int price, int count,
                         bool is_taker = false,
                         const std::string &fee_cost = "") {
  nlohmann::json msg;
  msg["type"] = "fill";
  msg["msg"]["order_id"] = order_id;
  msg["msg"]["market_ticker"] = ticker;
  msg["msg"]["outcome_side"] = side;
  msg["msg"]["yes_price_dollars"] = cents_to_dollars(price);
  msg["msg"]["count_fp"] = format_count(count);
  msg["msg"]["ts_ms"] = kFillTsMs;
  msg["msg"]["is_taker"] = is_taker;
  if (!fee_cost.empty()) {
    msg["msg"]["fee_cost"] = fee_cost;
  }
  return msg.dump();
}

std::string with_seq(const std::string &message, long long sid, long long seq) {
  auto parsed = nlohmann::json::parse(message);
  parsed["sid"] = sid;
  parsed["seq"] = seq;
  return parsed.dump();
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

  // [0] = fill channel sub, [1] = market sub
  ASSERT_EQ(ws_raw->sent_messages().size(), kFillPlusOneMarket);
  const auto market_sub = nlohmann::json::parse(ws_raw->sent_messages()[1]);
  EXPECT_EQ(market_sub["cmd"], "subscribe");
}

TEST_F(WebSocketClientTest, SubscribesToFillChannelOnConnect) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.run();

  ASSERT_GE(ws_raw->sent_messages().size(), kOneLevel);
  const auto fill_sub = nlohmann::json::parse(ws_raw->sent_messages()[0]);
  EXPECT_EQ(fill_sub["cmd"], "subscribe");
  const auto &channels = fill_sub["params"]["channels"];
  ASSERT_EQ(channels.size(), kOneLevel);
  EXPECT_EQ(channels[0], "fill");
  EXPECT_FALSE(fill_sub["params"].contains("market_ticker"));
}

TEST_F(WebSocketClientTest, SubscribeSendsCorrectTicker) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.subscribe(kTestTicker);
  client.run();

  ASSERT_EQ(ws_raw->sent_messages().size(), kFillPlusOneMarket);
  const auto market_sub = nlohmann::json::parse(ws_raw->sent_messages()[1]);
  const auto &tickers = market_sub["params"]["market_tickers"];
  ASSERT_EQ(tickers.size(), kOneLevel);
  EXPECT_EQ(tickers[0], kTestTicker);
}

TEST_F(WebSocketClientTest, SubscribeSendsOrderbookDeltaChannel) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.subscribe(kTestTicker);
  client.run();

  ASSERT_EQ(ws_raw->sent_messages().size(), kFillPlusOneMarket);
  const auto market_sub = nlohmann::json::parse(ws_raw->sent_messages()[1]);
  const auto &channels = market_sub["params"]["channels"];
  ASSERT_FALSE(channels.empty());
  EXPECT_EQ(channels[0], "orderbook_delta");
  EXPECT_FALSE(market_sub["params"].contains("use_yes_price"));
}

TEST_F(WebSocketClientTest, TwoMarketSubscribesSendThreeMessages) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  auto client = make_client(std::move(fake_ws));
  client.subscribe("KXBTCD");
  client.subscribe("KXETHU");
  client.run();

  // fill sub + 2 market subs = 3
  EXPECT_EQ(ws_raw->sent_messages().size(), kFillPlusTwoMarkets);
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
  EXPECT_EQ(received.yes[0].quantity,
            kalshi::Quantity::from_contracts(kYesBidQty));
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
  EXPECT_EQ(received.no[0].quantity,
            kalshi::Quantity::from_contracts(kNoBidQty));
}

TEST_F(WebSocketClientTest, SnapshotWithMultipleLevelsPreservesAll) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();

  nlohmann::json msg;
  msg["type"] = "orderbook_snapshot";
  msg["msg"]["market_ticker"] = kTestTicker;
  msg["msg"]["yes_dollars_fp"] =
      nlohmann::json::array({make_level(kYesBidPrice, kYesBidQty),
                             make_level(kSecondLevelPrice, kSecondLevelQty)});
  msg["msg"]["no_dollars_fp"] = nlohmann::json::array();
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
  kalshi::Quantity recv_qty{};
  client.on_orderbook_delta(
      [&](const std::string &ticker, kalshi::Side side,
          int price, // NOLINT(bugprone-easily-swappable-parameters)
          kalshi::Quantity qty) {
        recv_ticker = ticker;
        recv_side = side;
        recv_price = price;
        recv_qty = qty;
      });

  client.run();

  EXPECT_EQ(recv_ticker, kTestTicker);
  EXPECT_EQ(recv_side, kalshi::Side::Yes);
  EXPECT_EQ(recv_price, kYesBidPrice);
  EXPECT_EQ(recv_qty, kalshi::Quantity::from_contracts(kDeltaQty));
}

TEST_F(WebSocketClientTest, DeltaSideNoIsParsedCorrectly) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(
      delta_message(kTestTicker, "no", kNoBidPrice, kDeltaQty));

  auto client = make_client(std::move(fake_ws));

  kalshi::Side recv_side{kalshi::Side::Yes};
  client.on_orderbook_delta(
      [&](const std::string & /*ticker*/, kalshi::Side side, int /*price*/,
          kalshi::Quantity /*qty*/) { recv_side = side; });

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
  EXPECT_EQ(received.quantity, kalshi::Quantity::from_contracts(kFillCount));
  EXPECT_FALSE(received.is_taker); // default fill_message is maker (passive)
  EXPECT_DOUBLE_EQ(received.fee_cents, 0.0); // absent fee_cost defaults to zero
}

TEST_F(WebSocketClientTest, FillFeeCostParsedFromDollarsToCents) {
  constexpr double kExpectedFeeCents = 2.0; // "0.0200" dollars = 2.00 cents
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(fill_message("order-abc", kTestTicker, "yes",
                                       kFillPrice, kFillCount,
                                       /*is_taker=*/true, "0.0200"));

  auto client = make_client(std::move(fake_ws));

  kalshi::Fill received;
  client.on_fill([&](const kalshi::Fill &fill) { received = fill; });
  client.run();

  EXPECT_NEAR(received.fee_cents, kExpectedFeeCents, 1e-9);
}

TEST_F(WebSocketClientTest, ContiguousSeqDispatchesAllDeltas) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  constexpr long long kSid = 2;
  ws_raw->enqueue_message(
      with_seq(snapshot_message(kTestTicker, kYesBidPrice, kYesBidQty,
                                kNoBidPrice, kNoBidQty),
               kSid, 1));
  ws_raw->enqueue_message(with_seq(
      delta_message(kTestTicker, "yes", kYesBidPrice, kYesBidQty), kSid, 2));
  ws_raw->enqueue_message(with_seq(
      delta_message(kTestTicker, "yes", kYesBidPrice, kYesBidQty), kSid, 3));

  auto client = make_client(std::move(fake_ws));
  int deltas = 0;
  client.on_orderbook_delta([&deltas](const std::string &, kalshi::Side, int,
                                      kalshi::Quantity) { ++deltas; });
  client.run();

  EXPECT_EQ(deltas, 2);
  EXPECT_FALSE(ws_raw->close_requested());
}

TEST_F(WebSocketClientTest, SeqGapDiscardsDeltaAndRequestsReconnect) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  constexpr long long kSid = 2;
  ws_raw->enqueue_message(
      with_seq(snapshot_message(kTestTicker, kYesBidPrice, kYesBidQty,
                                kNoBidPrice, kNoBidQty),
               kSid, 1));
  ws_raw->enqueue_message(with_seq(
      delta_message(kTestTicker, "yes", kYesBidPrice, kYesBidQty), kSid, 2));
  constexpr long long kGappedSeq = 4;
  ws_raw->enqueue_message(
      with_seq(delta_message(kTestTicker, "yes", kYesBidPrice, kYesBidQty),
               kSid, kGappedSeq));

  auto client = make_client(std::move(fake_ws));
  int deltas = 0;
  client.on_orderbook_delta([&deltas](const std::string &, kalshi::Side, int,
                                      kalshi::Quantity) { ++deltas; });
  client.run();

  EXPECT_EQ(deltas, 1) << "the gapped delta must not reach the book";
  EXPECT_TRUE(ws_raw->close_requested());
}

TEST_F(WebSocketClientTest, MalformedDeltaSideIsDroppedNotMisparsed) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(
      delta_message(kTestTicker, "bogus", kYesBidPrice, kYesBidQty));

  auto client = make_client(std::move(fake_ws));
  int deltas = 0;
  client.on_orderbook_delta([&deltas](const std::string &, kalshi::Side, int,
                                      kalshi::Quantity) { ++deltas; });
  client.run();

  EXPECT_EQ(deltas, 0);
}

TEST_F(WebSocketClientTest, ThrowingCallbackIsContainedNotPropagated) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(
      fill_message("order-abc", kTestTicker, "yes", kFillPrice, kFillCount));

  auto client = make_client(std::move(fake_ws));
  client.on_fill([](const kalshi::Fill & /*fill*/) {
    throw std::runtime_error("callback boom");
  });

  // On the real WS thread an escaping exception would call std::terminate.
  // handle_message must contain it so the connection survives.
  EXPECT_NO_THROW(client.run());
}

TEST_F(WebSocketClientTest, FillIsTakerParsedCorrectly) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(fill_message("order-abc", kTestTicker, "yes",
                                       kFillPrice, kFillCount,
                                       /*is_taker=*/true));

  auto client = make_client(std::move(fake_ws));

  bool recv_is_taker = false;
  client.on_fill(
      [&](const kalshi::Fill &fill) { recv_is_taker = fill.is_taker; });
  client.run();

  EXPECT_TRUE(recv_is_taker);
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
      [&](const std::string &, kalshi::Side, int, kalshi::Quantity) {
        any_called = true;
      });
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

  // 2 connects × (fill sub + market sub) = 4 messages total.
  EXPECT_EQ(ws_raw->sent_messages().size(), kFourMessages);
}

TEST_F(WebSocketClientTest, DisconnectCallbackFiredOnDisconnect) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->trigger_disconnect();

  auto client = make_client(std::move(fake_ws));
  int disconnect_count = 0;
  client.on_disconnect([&disconnect_count]() { ++disconnect_count; });
  client.run();

  EXPECT_EQ(disconnect_count, 1);
}

TEST_F(WebSocketClientTest, ReconnectBackoffDoublesPerFailureAndCaps) {
  using std::chrono::milliseconds;
  constexpr milliseconds kBase{5000};
  constexpr milliseconds kCap{60000};
  EXPECT_EQ(kalshi::reconnect_backoff(kBase, 1), milliseconds{5000});
  EXPECT_EQ(kalshi::reconnect_backoff(kBase, 2), milliseconds{10000});
  EXPECT_EQ(kalshi::reconnect_backoff(kBase, 4), milliseconds{40000});
  EXPECT_EQ(kalshi::reconnect_backoff(kBase, 5), kCap);
  constexpr int kManyFailures = 100;
  EXPECT_EQ(kalshi::reconnect_backoff(kBase, kManyFailures), kCap);
  EXPECT_EQ(kalshi::reconnect_backoff(milliseconds{0}, 3), milliseconds{0});
  EXPECT_EQ(kalshi::reconnect_backoff(kBase, 0), kBase);
}

TEST_F(WebSocketClientTest, FailedHandshakesEscalateReconnectDelay) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  fake_ws->set_handshake_failure(true);
  constexpr std::chrono::milliseconds kBase{4};
  constexpr int kThreeReconnects = 3;
  constexpr std::chrono::milliseconds kEscalatedDelay{32};

  kalshi::Auth auth{kApiKey, kPemPrivateKey};
  kalshi::WebSocketClient client{auth, std::move(fake_ws), kWsUrl,
                                 kThreeReconnects, kBase};
  client.run();

  EXPECT_EQ(client.next_reconnect_delay(), kEscalatedDelay);
}

TEST_F(WebSocketClientTest, SuccessfulConnectResetsReconnectBackoff) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  constexpr std::chrono::milliseconds kBase{4};
  constexpr int kThreeReconnects = 3;

  kalshi::Auth auth{kApiKey, kPemPrivateKey};
  kalshi::WebSocketClient client{auth, std::move(fake_ws), kWsUrl,
                                 kThreeReconnects, kBase};
  client.run();

  EXPECT_EQ(client.next_reconnect_delay(), kBase);
}

TEST_F(WebSocketClientTest, LastMessageTimeUpdatesOnMessage) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->enqueue_message(snapshot_message(kTestTicker, kYesBidPrice,
                                           kYesBidQty, kNoBidPrice, kNoBidQty));

  auto client = make_client(std::move(fake_ws));
  const auto before = std::chrono::steady_clock::now();
  client.run();
  const auto after = std::chrono::steady_clock::now();

  const auto last = client.last_message_time();
  EXPECT_GE(last, before);
  EXPECT_LE(last, after);
}

TEST_F(WebSocketClientTest, HeartbeatRefreshesLastMessageTimeWithoutMessages) {
  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *ws_raw = fake_ws.get();
  ws_raw->trigger_heartbeat();

  auto client = make_client(std::move(fake_ws));
  const auto before = std::chrono::steady_clock::now();
  client.run();
  const auto after = std::chrono::steady_clock::now();

  const auto last = client.last_message_time();
  EXPECT_GE(last, before);
  EXPECT_LE(last, after);
}
