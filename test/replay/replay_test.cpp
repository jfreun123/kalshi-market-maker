#include "auth.hpp"
#include "fake_websocket.hpp"
#include "orderbook.hpp"
#include "websocket_client.hpp"

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int kRsaKeyBits = 2048;
constexpr int kExpectedFillCount = 2;
constexpr int kMinValidPrice = 1;
constexpr int kMaxValidPrice = 99;

std::string generate_test_private_pem() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C API
  EVP_PKEY *pkey = EVP_RSA_gen(static_cast<unsigned int>(kRsaKeyBits));
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  BUF_MEM *buf_mem = nullptr;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C API
  BIO_get_mem_ptr(bio, &buf_mem);
  std::string pem(buf_mem->data, buf_mem->length);
  BIO_free(bio);
  EVP_PKEY_free(pkey);
  return pem;
}

std::vector<std::string> load_jsonl(const std::string &path) {
  std::ifstream file{path};
  if (!file) {
    throw std::runtime_error{"Cannot open fixture: " + path};
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

kalshi::WebSocketClient
make_ws_client(const std::string &priv_pem,
               std::unique_ptr<kalshi::IWebSocket> transport) {
  const kalshi::Auth auth{"replay-test-key", priv_pem};
  return kalshi::WebSocketClient{
      auth, std::move(transport), "wss://test.example.com/ws",
      /*max_reconnects=*/0,
      /*reconnect_delay=*/std::chrono::milliseconds{0}};
}

// Validates one BBO level: present and price in [1, 99].
void assert_level_valid(const std::optional<kalshi::Level> &level,
                        const char *name) {
  ASSERT_TRUE(level.has_value()) << name << " should be set after snapshot";
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — ASSERT_TRUE guards
  EXPECT_GE(level->price_cents, kMinValidPrice);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_LE(level->price_cents, kMaxValidPrice);
}

void assert_valid_bbo(const kalshi::LocalOrderbook &book) {
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  assert_level_valid(bid, "best_bid");
  assert_level_valid(ask, "best_ask");
  if (bid.has_value() && ask.has_value()) {
    EXPECT_LT(bid->price_cents, ask->price_cents)
        << "bid must be strictly below ask";
  }
}

} // namespace

// Feeds the synthetic session fixture through the full WebSocketClient +
// LocalOrderbook stack and verifies the resulting state is correct.
TEST(ReplayTest, SyntheticSessionProducesValidOrderbookAndFills) {
  const std::string priv_pem = generate_test_private_pem();

  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *fake_ws_ptr = fake_ws.get();
  kalshi::WebSocketClient ws_client =
      make_ws_client(priv_pem, std::move(fake_ws));

  kalshi::LocalOrderbook book;
  int fill_count = 0;

  ws_client.on_orderbook_snapshot(
      [&book](const kalshi::Orderbook &snap) { book.apply_snapshot(snap); });
  ws_client.on_orderbook_delta(
      [&book](const std::string & /*ticker*/, kalshi::Side side, int price,
              int qty) { book.apply_delta(side, price, qty); });
  ws_client.on_fill(
      [&fill_count](const kalshi::Fill & /*fill*/) { ++fill_count; });

  // NOLINTNEXTLINE(modernize-raw-string-literal) — macro prevents raw literal
  for (const auto &msg :
       load_jsonl(KALSHI_FIXTURES_DIR "/session_synthetic.jsonl")) {
    fake_ws_ptr->enqueue_message(msg);
  }

  ws_client.run();

  EXPECT_EQ(fill_count, kExpectedFillCount);
  assert_valid_bbo(book);
}

// Verifies that malformed and unknown message types are silently dropped.
TEST(ReplayTest, MalformedMessagesAreDroppedGracefully) {
  const std::string priv_pem = generate_test_private_pem();

  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *fake_ws_ptr = fake_ws.get();
  kalshi::WebSocketClient ws_client =
      make_ws_client(priv_pem, std::move(fake_ws));

  int callback_count = 0;
  ws_client.on_orderbook_snapshot(
      [&callback_count](const kalshi::Orderbook &) { ++callback_count; });
  ws_client.on_orderbook_delta([&callback_count](const std::string &,
                                                 kalshi::Side, int,
                                                 int) { ++callback_count; });
  ws_client.on_fill(
      [&callback_count](const kalshi::Fill &) { ++callback_count; });

  fake_ws_ptr->enqueue_message("not json at all");
  fake_ws_ptr->enqueue_message(R"({"no_type_field": true})");
  fake_ws_ptr->enqueue_message(R"({"type":"unknown","msg":{}})");

  ws_client.run();

  EXPECT_EQ(callback_count, 0);
}
