// Full-stack replay integration test.
//
// Feeds a captured/synthetic exchange WebSocket session through the REAL
// production wiring (WebSocketClient parser → TradingSession → Quoter /
// OrderManager / RiskManager / Portfolio) with a PaperTransport standing in for
// the REST exchange. Asserts end-to-end invariants hold after the replay.
//
// This is the harness that validates real demo field shapes: drop a fixture
// captured from the demo environment (see --capture) into test/fixtures/ and a
// mismatch between the WS schema and the parser surfaces here as a broken
// orderbook or a missing fill — directly exercising the UAT field-shape check.

#include "auth.hpp"
#include "fake_websocket.hpp"
#include "order_manager.hpp"
#include "paper_transport.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"
#include "trading_session.hpp"
#include "websocket_client.hpp"

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kRsaKeyBits = 2048;
constexpr int kMinValidPrice = 1;
constexpr int kMaxValidPrice = 99;

// The synthetic fixture trades a single market; quantities are unambiguous:
// one YES fill of 5 and one NO fill of 3 → net position +5 - 3 = 2.
const std::string kFixtureTicker = "REPLAY-TICK";
constexpr int kExpectedNetPosition = 2;

const std::string kApiKey = "replay-test-key";
const std::string kBaseUrl = "https://demo-api.kalshi.co/trade-api/v2";
const std::string kWsUrl = "wss://demo-api.kalshi.co/trade-api/ws/v2";

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

} // namespace

// Builds the full production stack and replays the synthetic session through it
// once, in the constructor. Each TEST_F then asserts one end-state invariant.
// All members are public so the generated TEST_F subclasses reach them.
class ReplaySessionTest : public ::testing::Test {
public:
  ReplaySessionTest()
      : priv_pem_{generate_test_private_pem()},
        rest_{kalshi::Auth{kApiKey, priv_pem_},
              std::make_unique<kalshi::PaperTransport>(), kBaseUrl},
        order_mgr_{rest_}, risk_mgr_{kalshi::RiskLimits{}},
        quoter_{kalshi::QuoterConfig{}, order_mgr_, risk_mgr_},
        session_{std::vector<std::string>{kFixtureTicker}, order_mgr_,
                 risk_mgr_, quoter_} {
    replay();
  }

  void replay() {
    auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
    kalshi::FakeWebSocket *fake_ws_ptr = fake_ws.get();
    kalshi::WebSocketClient ws_client{
        kalshi::Auth{kApiKey, priv_pem_}, std::move(fake_ws), kWsUrl,
        /*max_reconnects=*/0,
        /*reconnect_delay=*/std::chrono::milliseconds{0}};

    // Production wiring: WS callbacks delegate to the TradingSession.
    ws_client.on_orderbook_snapshot(
        [this](const kalshi::Orderbook &snap) { session_.on_snapshot(snap); });
    ws_client.on_orderbook_delta(
        [this](const std::string &ticker, kalshi::Side side, int price,
               int qty) { session_.on_delta(ticker, side, price, qty); });
    ws_client.on_fill(
        [this](const kalshi::Fill &fill) { session_.on_fill(fill); });

    // NOLINTNEXTLINE(modernize-raw-string-literal) — macro prevents raw literal
    for (const auto &msg :
         load_jsonl(KALSHI_FIXTURES_DIR "/session_synthetic.jsonl")) {
      fake_ws_ptr->enqueue_message(msg);
    }
    ws_client.run();
  }

  std::string priv_pem_;
  kalshi::RestClient rest_;
  kalshi::OrderManager order_mgr_;
  kalshi::RiskManager risk_mgr_;
  kalshi::Quoter quoter_;
  kalshi::TradingSession session_;
};

// Snapshot + delta field shapes parse into a valid two-sided book.
TEST_F(ReplaySessionTest, ParsesSnapshotAndDeltasIntoValidBook) {
  const auto &books = session_.orderbooks();
  ASSERT_TRUE(books.contains(kFixtureTicker));
  const auto bid = books.at(kFixtureTicker).best_bid();
  const auto ask = books.at(kFixtureTicker).best_ask();
  ASSERT_TRUE(bid.has_value());
  ASSERT_TRUE(ask.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE
  EXPECT_GE(bid->price_cents, kMinValidPrice);
  EXPECT_LE(ask->price_cents, kMaxValidPrice);
  EXPECT_LT(bid->price_cents, ask->price_cents);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

// Fill field shapes parse and apply to local accounting.
TEST_F(ReplaySessionTest, ParsesFillsIntoLocalAccounting) {
  EXPECT_EQ(order_mgr_.net_position(kFixtureTicker), kExpectedNetPosition);
}

// The market data drove the quoter to place resting orders via the REST path —
// this is what regresses to zero when the paper/REST schema drifts.
TEST_F(ReplaySessionTest, MarketDataDrivesQuotePlacement) {
  EXPECT_FALSE(order_mgr_.open_orders().empty());
}

// A clean session does not spuriously trip any risk constraint.
TEST_F(ReplaySessionTest, CleanSessionDoesNotHaltRisk) {
  EXPECT_FALSE(risk_mgr_.is_halted());
}

// The portfolio read-model is computable over the replayed state.
TEST_F(ReplaySessionTest, PortfolioSnapshotIsComputable) {
  const auto snapshot = session_.portfolio_snapshot();
  EXPECT_GE(snapshot.total_notional_cents, 0.0);
}
