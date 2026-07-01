#include "ensure.hpp"
#include "fair_value.hpp"
#include "fake_transport.hpp"
#include "flow_imbalance.hpp"
#include "order_manager.hpp"
#include "orderbook.hpp"
#include "pricing_model.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"

#include <gtest/gtest.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
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
// Extreme long skew prices the raw ask at 4c — through the market bid of 51.
// The non-crossing clamp lifts it to the passive 52c (1c above the bid).
constexpr std::string_view kAskPriceExtremeClamp = R"("price":"0.5200")";
// Flow imbalance: default spread 4 → half 2 → bid 50; +2 imbalance → half 3 →
// bid 49.
constexpr std::string_view kBidPriceImbalanced = R"("price":"0.4900")";
constexpr int kImbalanceYesQty = 30;
constexpr int kImbalanceNoQty = 5;
// Spread floor: target 2 (half 1 → bid 51) is overridden by min_spread 8
// (half 4 → bid 48 at mid 52).
constexpr int kLowTargetSpread = 2;
constexpr int kHighMinSpread = 8;
constexpr std::string_view kBidPriceFloored = R"("price":"0.4800")";
// Maker fee: at fv 52 and γ=0.07, fee = round(0.07*0.52*0.48*100) = 2c, so the
// default spread 4 (half 2) widens to half 4 → bid 48 ("0.4800").
constexpr double kMakerFeeRate = 0.07;

constexpr int kObLevelQty = 100;
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
  fill.quantity = quantity;
  fill.timestamp =
      std::chrono::system_clock::time_point{std::chrono::nanoseconds{kTs1Ns}};
  order_mgr.record_fill(fill);
}

// A pricing model that returns NaN, to exercise the quoter's finiteness guard.
class NanModel : public kalshi::IPricingModel {
public:
  [[nodiscard]] double
  estimate(const kalshi::FairValueInput & /*input*/) const override {
    return std::numeric_limits<double>::quiet_NaN();
  }
};

// A pricing model that returns a fixed fair value, to drive the quoter to an
// aggressive price and exercise the non-crossing clamp deterministically.
class ConstantModel : public kalshi::IPricingModel {
public:
  explicit ConstantModel(double cents) : cents_{cents} {}
  [[nodiscard]] double
  estimate(const kalshi::FairValueInput & /*input*/) const override {
    return cents_;
  }

private:
  double cents_;
};

// Fair value of 70 with mid≈52 would price the bid at 68 — through a best ask
// of 53; the clamp must pull it back. 50 keeps both sides mid-book.
constexpr double kAggressiveFvCents = 70.0;
constexpr double kMidFvCents = 50.0;
// A book whose best bid sits at the ceiling, leaving no passive ask room.
constexpr int kYesBidAtCeiling = 99;
constexpr int kNoBid45 = 45; // best ask = 55

struct EnsureAborted : std::exception {};

// Routes ensure() failures to a thrown exception so the fail path is testable,
// recording whether the flatten hook ran; restores defaults on scope exit.
class EnsureAbortGuard {
public:
  EnsureAbortGuard() {
    kalshi::reset_panic_state();
    kalshi::set_panic_handler([this] { flattened_ = true; });
    kalshi::set_abort_fn([] { throw EnsureAborted{}; });
  }
  EnsureAbortGuard(const EnsureAbortGuard &) = delete;
  EnsureAbortGuard &operator=(const EnsureAbortGuard &) = delete;
  EnsureAbortGuard(EnsureAbortGuard &&) = delete;
  EnsureAbortGuard &operator=(EnsureAbortGuard &&) = delete;
  ~EnsureAbortGuard() {
    kalshi::set_panic_handler(nullptr);
    kalshi::set_abort_fn(nullptr);
    kalshi::reset_panic_state();
  }
  [[nodiscard]] bool flattened() const { return flattened_; }

private:
  bool flattened_ = false;
};

} // namespace

// ---- Test fixture ----

class QuoterTest : public ::testing::Test {
public:
  static void SetUpTestSuite() { kPemPrivateKey = generate_rsa_pem(); }
};

TEST_F(QuoterTest, NonFiniteFairValueFlattensAndAborts) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  auto *transport = transport_ptr.get();
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{},
                        kalshi::FairValueEngine{std::make_unique<NanModel>()},
                        order_mgr, risk_mgr};

  EnsureAbortGuard guard;
  // A NaN fair value would otherwise be cast to int (UB) and produce a garbage
  // quote — the quoter must flatten and crash instead of quoting.
  EXPECT_THROW(quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)),
               EnsureAborted);
  EXPECT_TRUE(guard.flattened());
  EXPECT_TRUE(transport->recorded_requests().empty());
}

// ---- Non-crossing clamp (stay strictly passive vs. the observed BBO) ----

TEST_F(QuoterTest, BidClampedToStayPassiveBelowMarketAsk) {
  // best_ask = 100 - 47 = 53. A fair value of 70 prices the raw bid at 68 —
  // through the ask. The clamp must pull it back to 52 (best_ask - 1).
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
  kalshi::Quoter quoter{kalshi::QuoterConfig{},
                        kalshi::FairValueEngine{std::make_unique<ConstantModel>(
                            kAggressiveFvCents)},
                        order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  // 52c = best_ask (53) - 1, not the raw 68 ("0.6800").
  EXPECT_NE(bid_body.find(R"("price":"0.5200")"), std::string::npos);
  EXPECT_EQ(bid_body.find(R"("price":"0.6800")"), std::string::npos);
}

TEST_F(QuoterTest, AskSkippedWhenMarketBidLeavesNoPassiveRoom) {
  // best_bid = 99 leaves no room for a passive ask (min passive would be 100).
  // The ask side is skipped; the bid still rests.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{
      kalshi::QuoterConfig{},
      kalshi::FairValueEngine{std::make_unique<ConstantModel>(kMidFvCents)},
      order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBidAtCeiling, kNoBid45));

  ASSERT_EQ(transport.recorded_requests().size(), 1U);
  EXPECT_NE(transport.recorded_requests().at(0).body.find("\"side\":\"bid\""),
            std::string::npos);
}

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
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  quoter.update(kTicker, make_ob(kYesBid55, kNoBid55));

  // 2 POSTs + 2 DELETEs + 2 POSTs = 6 total requests.
  EXPECT_EQ(transport.recorded_requests().size(), 6U);
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

TEST_F(QuoterTest, RepriceReplacesFilledOrderInsteadOfCancellingDeadId) {
  // A resting bid that fully fills is erased from the order manager's open
  // orders, but the quoter still tracks its id. On the next reprice the quoter
  // must recognise the id is dead (gone from open_orders) and place a fresh
  // bid — NOT keep issuing cancels against the filled id, which the exchange
  // rejects forever (observed live: 127 failed cancels on one filled order).
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  // update 1 (mid=52): POST bid order-001, POST ask order-002.
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  // update 2 (mid=55): fresh bid POST (order-003), then ask reprice —
  // DELETE ask order-002, POST ask order-004. No DELETE for the filled bid.
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId3, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, "{}"}); // DELETE ask order-002
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  ASSERT_EQ(transport.recorded_requests().size(), 2U);

  // The bid (order-001) fills completely — order_mgr erases it from open
  // orders.
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kalshi::QuoterConfig::kDefaultQuoteSize);

  quoter.update(kTicker, make_ob(kYesBid55, kNoBid55));

  // The filled bid must never be the target of a cancel.
  for (const auto &request : transport.recorded_requests()) {
    const bool cancels_filled_bid =
        request.method == "DELETE" &&
        request.url.find(kOrderId1) != std::string::npos;
    EXPECT_FALSE(cancels_filled_bid)
        << "must not cancel the already-filled order " << kOrderId1;
  }
  // Instead the quoter re-places the bid: request index 2 is a fresh bid POST.
  ASSERT_EQ(transport.recorded_requests().size(), 5U);
  const auto &fresh_bid = transport.recorded_requests().at(2);
  EXPECT_EQ(fresh_bid.method, "POST");
  EXPECT_NE(fresh_bid.body.find("\"side\":\"bid\""), std::string::npos);
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
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // bid=50, ask=54
  quoter.update(kTicker,
                make_ob(kYesBid70, kNoBid70)); // desired bid=68 crosses ask

  // POST bid + POST ask + DELETE bid + DELETE ask + POST ask = 5
  EXPECT_EQ(transport.recorded_requests().size(), 5U);
  // The last POST should be an ask (side=ask), not a bid.
  const std::string &last_body = transport.recorded_requests().back().body;
  EXPECT_NE(last_body.find("\"side\":\"ask\""), std::string::npos);
}

TEST_F(QuoterTest, AskAlwaysHigherThanBidWithExtremeInventory) {
  // pos=+1000: inv_skew=50 → raw bid=0→1 (range-clamped), raw ask=4. The raw
  // ask is through the market bid (51), so the non-crossing clamp lifts it to
  // the passive 52. Verifies ask(52) > bid(1) and neither side crosses.
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
  // kExpectedNoAskExtremeClamp = 96 → YES complement = 4 → "0.0400".
  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find("\"side\":\"ask\""), std::string::npos);
  EXPECT_NE(ask_body.find(std::string(kAskPriceExtremeClamp)),
            std::string::npos);
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
