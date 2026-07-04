#include "fake_transport.hpp"
#include "flow_imbalance.hpp"
#include "order_manager.hpp"
#include "orderbook.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"

#include <gtest/gtest.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <chrono>
#include <memory>
#include <string>

// ---- Test constants ----

namespace {

// Orderbook YES/NO bid levels that yield specific mid-prices.
// mid = (yes_bid + (100 - no_bid)) / 2
constexpr int kYesBid52 = 51;
constexpr int kNoBid52 = 47; // mid = (51+53)/2 = 52
constexpr int kYesBid53 = 51;
constexpr int kNoBid53 = 45; // mid = (51+55)/2 = 53
constexpr int kYesBid55 = 51;
constexpr int kNoBid55 = 41; // mid = (51+59)/2 = 55
// mid=70: desired bid=68, desired ask=72. After update(52), current_ask=54.
// 68 >= 54 → bid would cross own ask — guard should suppress bid.
constexpr int kYesBid70 = 67;
constexpr int kNoBid70 = 27; // mid = (67+73)/2 = 70

// Inventory skew positions (20 = one skew unit, 1000 = extreme clamp).
constexpr int kInventoryPosition = 20;
constexpr int kExtremeLongPosition = 1'000;

// V2 request body expected price strings (YES dimension, "price":"X.XXXX").
constexpr std::string_view kBidPriceMid52 = R"("price":"0.5000")";
constexpr std::string_view kAskPriceMid52 =
    R"("price":"0.5400")"; // YES=100-46=54
constexpr std::string_view kBidPriceMid52Long20 = R"("price":"0.4900")";
constexpr std::string_view kBidPriceMid52Short20 = R"("price":"0.5100")";
constexpr std::string_view kBidPriceExtremeClamp = R"("price":"0.0100")";
constexpr std::string_view kAskPriceExtremeClamp = R"("price":"0.5200")";
// Flow imbalance: default spread 4 → half 2 → bid 50; +2 imbalance → half 3 →
// bid 49.
constexpr std::string_view kBidPriceImbalanced = R"("price":"0.4900")";
constexpr kalshi::Quantity kImbalanceYesQty =
    kalshi::Quantity::from_contracts(30);
constexpr kalshi::Quantity kImbalanceNoQty =
    kalshi::Quantity::from_contracts(5);
// Spread floor: target 2 (half 1 → bid 51) is overridden by min_spread 8
// (half 4 → bid 48 at mid 52).
constexpr int kLowTargetSpread = 2;
constexpr int kHighMinSpread = 8;
constexpr std::string_view kBidPriceFloored = R"("price":"0.4800")";
constexpr int kOddMinSpread = 3;
constexpr std::string_view kBidPriceOddFloored = R"("price":"0.5000")";
// Maker fee: at fv 52 and γ=0.07, fee = round(0.07*0.52*0.48*100) = 2c, so the
// default spread 4 (half 2) widens to half 4 → bid 48 ("0.4800").
constexpr double kMakerFeeRate = 0.07;

constexpr auto kJustUnderMinRest =
    std::chrono::milliseconds{kalshi::QuoterConfig::kDefaultMinRestMs - 1};
constexpr auto kMinRestElapsed =
    std::chrono::milliseconds{kalshi::QuoterConfig::kDefaultMinRestMs};

constexpr kalshi::Quantity kObLevelQty = kalshi::Quantity::from_contracts(100);
constexpr int kFillPrice = 52;
constexpr long long kTs1Ns = 1'000'000LL;
constexpr int kHttpOk = 200;

const std::string kTicker = "KXBTCD";
const std::string kOrderId1 = "order-001";
const std::string kOrderId2 = "order-002";
const std::string kOrderId3 = "order-003";
const std::string kOrderId4 = "order-004";
const std::string kApiKey = "test-key-id";
const std::string kBaseUrl = "https://trading-api.kalshi.com/trade-api/v2";

// RSA key generated once per test suite.
std::string
    kPemPrivateKey; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

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

// Builds the V2 minimal response body that RestClient expects for a placed
// order.
std::string order_json(const std::string &order_id, int qty) {
  return R"({"order_id":")" + order_id +
         R"(","fill_count":"0.00","remaining_count":")" + std::to_string(qty) +
         R"(.00","ts_ms":1718000000000})";
}

// Builds a LocalOrderbook with the given YES and NO bid levels.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kalshi::LocalOrderbook make_ob(int yes_bid_cents, int no_bid_cents) {
  kalshi::Orderbook snap;
  snap.ticker = kTicker;
  snap.yes = {{yes_bid_cents, kObLevelQty}};
  snap.no = {{no_bid_cents, kObLevelQty}};
  kalshi::LocalOrderbook book;
  book.apply_snapshot(snap);
  return book;
}

// Records a single fill directly into an OrderManager (no REST call needed).
void record_position_fill(kalshi::OrderManager &order_mgr,
                          const std::string &order_id, kalshi::Side side,
                          int quantity) {
  kalshi::Fill fill;
  fill.order_id = order_id;
  fill.market_ticker = kTicker;
  fill.side = side;
  fill.price_cents = kFillPrice;
  fill.quantity = kalshi::Quantity::from_contracts(quantity);
  fill.timestamp = std::chrono::system_clock::time_point{
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::nanoseconds{kTs1Ns})};
  order_mgr.record_fill(fill);
}

} // namespace

// ---- Test fixture ----

class QuoterTest : public ::testing::Test {
public:
  static void SetUpTestSuite() { kPemPrivateKey = generate_rsa_pem(); }
};

// ---- Tests ----

TEST_F(QuoterTest, PlacesBidAndAskOnFirstUpdate) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // Two POST requests: one YES buy (bid) and one NO buy (ask).
  EXPECT_EQ(transport.recorded_requests().size(), 2U);
}

TEST_F(QuoterTest, BidPriceCalculatedFromMidAndSpread) {
  // mid=52, spread=4, half_spread=2, pos=0 → bid = round(52-2) = 50.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // kExpectedBidMid52 = 50 → "0.5000" in YES dimension.
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(std::string(kBidPriceMid52)), std::string::npos);
}

TEST_F(QuoterTest, AskNoPriceCalculatedFromMidAndSpread) {
  // mid=52, spread=4, ask=54, NO price = 100-54 = 46 → YES price = 54 =
  // "0.5400".
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // kExpectedNoAskMid52 = 46 → YES complement = 54 = "0.5400".
  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find("\"side\":\"ask\""), std::string::npos);
  EXPECT_NE(ask_body.find(std::string(kAskPriceMid52)), std::string::npos);
}

TEST_F(QuoterTest, NoOrdersPlacedWhenObHasNoBbo) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  kalshi::LocalOrderbook empty_ob;
  quoter.update(kTicker, empty_ob);

  EXPECT_TRUE(transport.recorded_requests().empty());
}

TEST_F(QuoterTest, NoOrdersPlacedWhenRiskHalted) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  risk_mgr.halt();
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  EXPECT_TRUE(transport.recorded_requests().empty());
}

TEST_F(QuoterTest, CrossedVisibleBookKeepsRestingQuotes) {
  // Run-3 anomaly (finding, item 43): during fast sweeps the book flickers
  // crossed (best ask below best bid); pricing off it drags the quote to
  // garbage (yes@28 in a 46-52 market). A crossed visible book means the
  // data is mid-transition — keep the resting quotes and wait.
  constexpr int kCrossedYesBid = 52;
  constexpr int kCrossedNoBid = 60;

  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  ASSERT_EQ(transport.recorded_requests().size(), 2U);

  quoter.update(kTicker, make_ob(kCrossedYesBid, kCrossedNoBid));

  EXPECT_EQ(transport.recorded_requests().size(), 2U)
      << "crossed book (bid 52 >= ask 40) must not trigger cancel/replace";
}

TEST_F(QuoterTest, BidRoundsDownAtFractionalFairValue) {
  // yes 51@4 / no 45@6 -> best ask 55 (size 6): micro = (51*6 + 55*4)/10 =
  // 52.6. Raw bid = 50.6: rounding to nearest would quote 51, giving away
  // 0.4c of the configured half-spread. The maker's bid must round DOWN.
  constexpr int kBidLevelPrice = 51;
  constexpr int kNoLevelPrice = 45;
  const kalshi::Quantity kBidSize = kalshi::Quantity::from_contracts(4);
  const kalshi::Quantity kAskSize = kalshi::Quantity::from_contracts(6);

  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  kalshi::Orderbook snap;
  snap.ticker = kTicker;
  snap.yes = {{kBidLevelPrice, kBidSize}};
  snap.no = {{kNoLevelPrice, kAskSize}};
  kalshi::LocalOrderbook book;
  book.apply_snapshot(snap);
  quoter.update(kTicker, book);

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(R"("price":"0.5000")"), std::string::npos)
      << "bid at fv 52.6 must floor to 50, not round to 51: " << bid_body;
}

TEST_F(QuoterTest, AskRoundsUpAtFractionalFairValue) {
  // yes 51@7 / no 46@8 -> best ask 54 (size 8): micro = (51*8 + 54*7)/15 =
  // 52.4. Raw ask = 54.4: rounding to nearest would quote 54, giving away
  // 0.4c. The maker's ask must round UP (NO order at 45 -> YES 55).
  constexpr int kBidLevelPrice = 51;
  constexpr int kNoLevelPrice = 46;
  const kalshi::Quantity kBidSize = kalshi::Quantity::from_contracts(7);
  const kalshi::Quantity kAskSize = kalshi::Quantity::from_contracts(8);

  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  kalshi::Orderbook snap;
  snap.ticker = kTicker;
  snap.yes = {{kBidLevelPrice, kBidSize}};
  snap.no = {{kNoLevelPrice, kAskSize}};
  kalshi::LocalOrderbook book;
  book.apply_snapshot(snap);
  quoter.update(kTicker, book);

  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find(R"("price":"0.5500")"), std::string::npos)
      << "ask at fv 52.4 must ceil to 55, not round to 54: " << ask_body;
}

TEST_F(QuoterTest, QuoteNotReplacedWithinRepriceThreshold) {
  // First update (mid=52): bid=50, ask=54.
  // Second update (mid=53): desired bid=51, ask=55; diff=1 = threshold → no
  // replace.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  quoter.update(kTicker, make_ob(kYesBid53, kNoBid53)); // diff=1, no reprice

  EXPECT_EQ(transport.recorded_requests().size(), 2U);
}

TEST_F(QuoterTest, QuoteReplacedWhenExceedingRepriceThreshold) {
  // First update (mid=52): bid=50, ask=54.
  // Second update (mid=55): bid=53, ask=57; diff=3 > 1 → cancel + replace both.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  // Second update sequence: DELETE bid1, POST bid2, DELETE ask1, POST ask2.
  transport.enqueue({kHttpOk, "{}"}); // DELETE bid1 — cancel_order ignores body
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId3, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, "{}"}); // DELETE ask1 — cancel_order ignores body
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  *clock_now += kMinRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid55, kNoBid55));

  // 2 POSTs + 2 DELETEs + 2 POSTs = 6 total requests.
  EXPECT_EQ(transport.recorded_requests().size(), 6U);
}

TEST_F(QuoterTest, RepriceSuppressedWhileQuoteYoungerThanMinRest) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  *clock_now += kJustUnderMinRest;
  quoter.update(kTicker, make_ob(kYesBid55, kNoBid55));

  EXPECT_EQ(transport.recorded_requests().size(), 2U)
      << "a 3c move must not cancel a quote that has rested < min_rest_ms";
}

TEST_F(QuoterTest, RepriceResumesOnceQuoteHasRestedMinRest) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, "{}"});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId3, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, "{}"});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  *clock_now += kMinRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid55, kNoBid55));

  EXPECT_EQ(transport.recorded_requests().size(), 6U)
      << "after resting min_rest_ms the quote must reprice as before";
}

TEST_F(QuoterTest, RepriceIgnoresOwnRestingQuotesEchoedInBook) {
  // Thin book (both sides 1 lot): mid = micro = 35 → bid=33, ask=37 (NO 63).
  // The exchange then echoes our resting 10-lot bid at 33 as the new best bid
  // while the filled ask side reverts. Priced off the raw echo, micro jumps to
  // (33*1 + 40*10)/11 ≈ 39.4 and the quoter cancels its own bid — the
  // self-referential churn oscillator (finding D4). Priced off the book minus
  // our own orders, fair value stays 35 and the resting bid is left alone.
  constexpr int kThinYesBid = 30;
  constexpr int kThinNoBid = 60;
  constexpr int kOwnBidPrice = 33;
  const kalshi::Quantity kThinQty = kalshi::Quantity::from_contracts(1);
  const kalshi::Quantity kOwnQty =
      kalshi::Quantity::from_contracts(kalshi::QuoterConfig::kDefaultQuoteSize);

  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId3, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, "{}"});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  kalshi::Orderbook thin;
  thin.ticker = kTicker;
  thin.yes = {{kThinYesBid, kThinQty}};
  thin.no = {{kThinNoBid, kThinQty}};
  kalshi::LocalOrderbook book;
  book.apply_snapshot(thin);
  quoter.update(kTicker, book);
  ASSERT_EQ(transport.recorded_requests().size(), 2U);

  *clock_now += kMinRestElapsed;
  record_position_fill(order_mgr, kOrderId2, kalshi::Side::No,
                       kalshi::QuoterConfig::kDefaultQuoteSize);
  quoter.forget_order(kTicker, kOrderId2);

  kalshi::Orderbook echo;
  echo.ticker = kTicker;
  echo.yes = {{kThinYesBid, kThinQty}, {kOwnBidPrice, kOwnQty}};
  echo.no = {{kThinNoBid, kThinQty}};
  kalshi::LocalOrderbook echo_book;
  echo_book.apply_snapshot(echo);
  quoter.update(kTicker, echo_book);

  EXPECT_EQ(transport.recorded_requests().size(), 3U)
      << "only the filled ask may be re-placed; the echoed bid must rest";
}

TEST_F(QuoterTest, ResetQuotesForgetsLiveStateSoNextUpdatePlacesFresh) {
  // After an out-of-band flatten (risk halt / disconnect cancels the resting
  // orders), the quoter must be told to forget them. Otherwise it believes its
  // quotes are still live, tries to cancel dead ids, and never re-quotes.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId3, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // 2 POSTs (bid + ask)
  ASSERT_EQ(transport.recorded_requests().size(), 2U);

  quoter.reset_quotes();

  // Same mid: without the reset the quoter would think it is already quoting at
  // the right price and do nothing; after the reset it places a fresh pair.
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  EXPECT_EQ(transport.recorded_requests().size(), 4U);
  EXPECT_EQ(transport.recorded_requests().at(2).method, "POST");
  EXPECT_EQ(transport.recorded_requests().at(3).method, "POST");
}

TEST_F(QuoterTest, LongPositionShiftsBidDown) {
  // pos=+20: inv_skew=1.0 → bid = round(52-2-1) = 49, ask = round(52+2-1) = 53.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kInventoryPosition);

  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // kExpectedBidMid52Long20 = 49 → "0.4900" in YES dimension.
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(std::string(kBidPriceMid52Long20)),
            std::string::npos);
}

TEST_F(QuoterTest, ShortPositionShiftsBidUp) {
  // pos=-20: inv_skew=-1.0 → bid = round(52-2+1) = 51, ask = round(52+2+1)
  // = 55.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::No,
                       kInventoryPosition);

  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // kExpectedBidMid52Short20 = 51 → "0.5100" in YES dimension.
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(std::string(kBidPriceMid52Short20)),
            std::string::npos);
}

TEST_F(QuoterTest, SelfCrossGuardSkipsBidWhenItWouldCrossOwnAsk) {
  // First update (mid=52): bid=50, ask=54 (current_ask_cents=54).
  // Second update (mid=70): desired bid=68 >= current_ask 54 → bid suppressed.
  // Only ask is repriced: DELETE ask, POST ask@72. Bid stays cancelled.
  // Expected requests: POST bid, POST ask, DELETE bid, DELETE ask, POST ask
  // = 5.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  // Initial: POST bid order-001, POST ask order-002.
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  // Reprice: DELETE bid order-001.
  transport.enqueue(
      {kHttpOk, R"({"order_id":"order-001","reduced_by":"5.00","ts_ms":0})"});
  // Bid suppressed — no POST bid here.
  // Ask reprice: DELETE ask order-002, POST ask order-003.
  transport.enqueue(
      {kHttpOk, R"({"order_id":"order-002","reduced_by":"5.00","ts_ms":0})"});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId3, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // bid=50, ask=54
  *clock_now += kMinRestElapsed;
  quoter.update(kTicker,
                make_ob(kYesBid70, kNoBid70)); // desired bid=68 crosses ask

  // POST bid + POST ask + DELETE bid + DELETE ask + POST ask = 5
  EXPECT_EQ(transport.recorded_requests().size(), 5U);
  // The last POST should be an ask (side=ask), not a bid.
  const std::string &last_body = transport.recorded_requests().back().body;
  EXPECT_NE(last_body.find("\"side\":\"ask\""), std::string::npos);
}

TEST_F(QuoterTest, AskAlwaysHigherThanBidWithExtremeInventory) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kExtremeLongPosition);

  // This test verifies clamping math (ask > bid at extreme skew), not the
  // price-range gate — open the band to the full valid range so the clamped
  // quotes are placed.
  constexpr int kWidestBandMin = 1;
  constexpr int kWidestBandMax = 99;
  kalshi::RiskLimits limits;
  limits.min_quote_price_cents = kWidestBandMin;
  limits.max_quote_price_cents = kWidestBandMax;
  kalshi::RiskManager risk_mgr{limits};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  // kExpectedBidExtremeClamp = 1 → "0.0100".
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(std::string(kBidPriceExtremeClamp)),
            std::string::npos);
  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find("\"side\":\"ask\""), std::string::npos);
  EXPECT_NE(ask_body.find(std::string(kAskPriceExtremeClamp)),
            std::string::npos);
}

TEST_F(QuoterTest, BidClampedToStayPassiveBelowMarketAsk) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::No,
                       kExtremeLongPosition);

  constexpr int kWidestBandMin = 1;
  constexpr int kWidestBandMax = 99;
  kalshi::RiskLimits limits;
  limits.min_quote_price_cents = kWidestBandMin;
  limits.max_quote_price_cents = kWidestBandMax;
  kalshi::RiskManager risk_mgr{limits};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_FALSE(transport.recorded_requests().empty());
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(R"("price":"0.5200")"), std::string::npos);
  EXPECT_EQ(bid_body.find(R"("price":"0.9800")"), std::string::npos);
}

TEST_F(QuoterTest, ImbalancedFlowWidensSpread) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  // Heavy one-sided flow → imbalanced (30 vs 5, ratio 6 > 2, vol 35 ≥ 20).
  kalshi::FlowImbalanceGuard flow_guard{kalshi::FlowImbalanceConfig{}};
  flow_guard.record_fill(kTicker, kalshi::Side::Yes, kImbalanceYesQty);
  flow_guard.record_fill(kTicker, kalshi::Side::No, kImbalanceNoQty);
  ASSERT_TRUE(flow_guard.is_imbalanced(kTicker));

  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr,
                        &flow_guard};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // mid 52

  // Spread 4 → bid 50 normally; +2 imbalance spread → half 3 → bid 49.
  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceImbalanced)), std::string::npos);
}

TEST_F(QuoterTest, SpreadFloorWidensTooTightTarget) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  // Target spread 2 would quote bid 51, but the min-spread floor of 8 forces
  // it.
  kalshi::QuoterConfig config;
  config.target_spread_cents = kLowTargetSpread;
  config.min_spread_cents = kHighMinSpread;
  kalshi::Quoter quoter{config, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // mid 52

  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceFloored)), std::string::npos);
}

TEST_F(QuoterTest, OddSpreadFloorRoundsHalfSpreadUp) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  kalshi::QuoterConfig config;
  config.target_spread_cents = kLowTargetSpread;
  config.min_spread_cents = kOddMinSpread;
  kalshi::Quoter quoter{config, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceOddFloored)), std::string::npos)
      << "min_spread 3 truncated to half-spread 1 quotes a 2c spread, "
         "violating the floor";
}

TEST_F(QuoterTest, MakerFeeWidensSpread) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  // Default spread 4 → bid 50; a 2c maker fee at fv 52 widens it → bid 48.
  kalshi::QuoterConfig config;
  config.maker_fee_rate = kMakerFeeRate;
  kalshi::Quoter quoter{config, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // mid 52

  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceFloored)), std::string::npos);
}
