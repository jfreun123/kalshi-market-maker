#pragma once
// Shared fixture, constants, and helpers for the Quoter test suites
// (quoter_test.cpp: pricing/skew/shading; quoter_reprice_test.cpp:
// hysteresis/fade/EMA behavior).

#include "analytics.hpp"
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
#include <vector>

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

// Inventory skew positions (LMSR log-odds: b = 100/ln(9) ≈ 45.5 contracts
// from max_position=100 and band edge 90c, so q=20 shifts fv 52 → 41.11 and
// q=−20 shifts fv 52 → 62.70; 1000 = extreme clamp).
constexpr int kInventoryPosition = 15; // below the 2x-quote-size cap
constexpr int kExtremeLongPosition = 1'000;

// V2 request body expected price strings (YES dimension, "price":"X.XXXX").
constexpr std::string_view kBidPriceMid52 = R"("price":"0.5000")";
constexpr std::string_view kAskPriceMid52 =
    R"("price":"0.5400")"; // YES=100-46=54
constexpr std::string_view kBidPriceMid52Long20 =
    R"("price":"0.4800")"; // b floored at 25×quote_size=250: q=15 → res ≈ 50.5
constexpr std::string_view kAskPriceMid52Long20 =
    R"("price":"0.5200")"; // unwind ask ceil(res 50.5)=51, passive-clamped to
                           // 52
constexpr std::string_view kBidPriceMid52Short20 =
    R"("price":"0.5200")"; // q=−15: unwind bid floor(res 53.5)=53 → clamp 52
constexpr std::string_view kBidPriceExtremeClamp = R"("price":"0.0100")";
constexpr std::string_view kAskPriceExtremeClamp = R"("price":"0.5200")";
// Flow imbalance: default spread 4 → half 2 → bid 50; +2 imbalance → half 3 →
// bid 49.
constexpr std::string_view kBidPriceImbalanced =
    R"("price":"0.4800")"; // widen (half 2→3) and lean −1c (takers bought NO)
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
constexpr auto kFadeRestElapsed =
    std::chrono::milliseconds{kalshi::QuoterConfig::kDefaultFadeRestMs};
constexpr auto kJustUnderFadeRest =
    std::chrono::milliseconds{kalshi::QuoterConfig::kDefaultFadeRestMs - 1};
constexpr int kYesBid54 = 51;
constexpr int kNoBid54 = 43;
constexpr int kYesBid49 = 47;
constexpr int kNoBid49 = 49;
constexpr int kYesBid47 = 45;
constexpr int kNoBid47 = 51;
constexpr int kYesBid57 = 55;
constexpr int kNoBid57 = 41;
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

// Official-docs amend response: same or new order_id, remaining/fill counts.
[[maybe_unused]] std::string amend_json(const std::string &order_id) {
  return R"({"order_id":")" + order_id +
         R"(","remaining_count":"10.00","fill_count":"0.00","ts_ms":1715793690123})";
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
[[maybe_unused]] void record_position_fill(kalshi::OrderManager &order_mgr,
                                           const std::string &order_id,
                                           kalshi::Side side, int quantity) {
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
