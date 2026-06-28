#include "fake_transport.hpp"
#include "ticker_scanner.hpp"

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <memory>
#include <string>

// ---- Constants ----

namespace {

constexpr int kRsaKeyBits = 2048;
constexpr std::string_view kTestBaseUrl = "https://test.kalshi.co/trade-api/v2";
constexpr int kHttpOk = 200;

// Fixed "now" for deterministic days_to_close calculations.
// Markets use close_times relative to 2026-06-21T00:00:00Z.
// 2026-06-21T00:00:00Z: 2025-06-21 (1750464000) + 365 days (2026 non-leap)
const auto kTestNow =
    std::chrono::system_clock::from_time_t(1782000000); // 2026-06-21

constexpr double kHighVolume = 200000.0;
constexpr int kScanTopN = 20;
constexpr int kScanTopTwo = 2;
constexpr double kCustomMinVolumeUsd = 150000.0;

// ---- Market JSON building blocks ----

// Good market: mid=50c, spread=4c, 5 days out, high volume, Financials
constexpr std::string_view kMarketGoodA =
    R"({"ticker":"KXFED-A","title":"Fed holds at 5.25%?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

// Good market: mid=49.5c, spread=5c, 4 days out, higher volume, Financials
constexpr std::string_view kMarketGoodB =
    R"({"ticker":"KXCPI-B","title":"CPI above 3%?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4700","yes_ask_dollars":"0.5200",)"
    R"("volume_fp":"400000.00","close_time":"2026-06-25T00:00:00Z"})";

// Crypto market: mid=50c, spread=4c, 5 days out, mid volume
constexpr std::string_view kMarketCrypto =
    R"({"ticker":"KXBTCD-C","title":"Bitcoin above $70k?","category":"Crypto",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_fp":"100000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: price too low (mid=7.5c — deep longshot)
constexpr std::string_view kMarketDeepLongshot =
    R"({"ticker":"KXLONG-D","title":"Longshot event?","category":"Other",)"
    R"("status":"active","yes_bid_dollars":"0.0500","yes_ask_dollars":"0.1000",)"
    R"("volume_fp":"500000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: price too high (mid=92.5c)
constexpr std::string_view kMarketDeepFavorite =
    R"({"ticker":"KXFAV-E","title":"Near-certain event?","category":"Other",)"
    R"("status":"active","yes_bid_dollars":"0.9000","yes_ask_dollars":"0.9500",)"
    R"("volume_fp":"500000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: volume too low
constexpr std::string_view kMarketLowVolume =
    R"({"ticker":"KXLOW-F","title":"Illiquid market?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_fp":"500.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: closes in 40 days (past max_days_to_close=10)
constexpr std::string_view kMarketTooFarOut =
    R"({"ticker":"KXFAR-G","title":"Far future event?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_fp":"200000.00","close_time":"2026-07-31T00:00:00Z"})";

// Filtered: not active
constexpr std::string_view kMarketClosed =
    R"({"ticker":"KXCLS-H","title":"Already closed?","category":"Financials",)"
    R"("status":"closed","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_fp":"200000.00","close_time":"2026-06-22T00:00:00Z"})";

// Filtered: spread too wide (>10c)
constexpr std::string_view kMarketWideSpread =
    R"({"ticker":"KXWIDE-I","title":"Wide spread market?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4000","yes_ask_dollars":"0.6000",)"
    R"("volume_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: spread too narrow (<3c) — indicates locked/crossed book
constexpr std::string_view kMarketNarrowSpread =
    R"({"ticker":"KXNARR-J","title":"Narrow spread market?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4900","yes_ask_dollars":"0.5000",)"
    R"("volume_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: closes in 0.3 days (too soon)
constexpr std::string_view kMarketTooSoon =
    R"({"ticker":"KXSOON-K","title":"Closing today?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_fp":"200000.00","close_time":"2026-06-21T07:00:00Z"})";

std::string
make_markets_response(std::initializer_list<std::string_view> markets) {
  std::string body = R"({"markets":[)";
  bool first = true;
  for (const auto &market : markets) {
    if (!first) {
      body += ',';
    }
    body += market;
    first = false;
  }
  body += R"(],"cursor":""})";
  return body;
}

std::string generate_test_private_key() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C API
  EVP_PKEY *pkey = EVP_RSA_gen(static_cast<unsigned int>(kRsaKeyBits));
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  BUF_MEM *buf_mem = nullptr;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C macro
  BIO_get_mem_ptr(bio, &buf_mem);
  std::string result(buf_mem->data, buf_mem->length);
  BIO_free(bio);
  EVP_PKEY_free(pkey);
  return result;
}

} // namespace

// ---- Fixture ----

class TickerScannerTest : public ::testing::Test {
  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes):
  // GTest requires protected members accessible in TEST_F bodies.
protected:
  static void SetUpTestSuite() { s_priv_pem_ = generate_test_private_key(); }

  [[nodiscard]] static std::pair<kalshi::RestClient, FakeTransport *>
  make_client_with_transport() {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *transport_raw = transport.get();
    kalshi::RestClient client{kalshi::Auth{"test-api-key", s_priv_pem_},
                              std::move(transport), std::string(kTestBaseUrl)};
    return {std::move(client), transport_raw};
  }

  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  static std::string s_priv_pem_; // NOLINT — GTest fixture static
};

std::string TickerScannerTest::s_priv_pem_; // NOLINT — GTest fixture static

// ---- Tests ----

TEST_F(TickerScannerTest, ScanReturnsGoodCandidates) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk,
       make_markets_response({kMarketGoodA, kMarketGoodB, kMarketCrypto})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 3U);
}

TEST_F(TickerScannerTest, ScanFiltersDeepLongshotMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketDeepLongshot})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanFiltersDeepFavoriteMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketDeepFavorite})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanFiltersLowVolumeMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketLowVolume})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanFiltersTooFarOutMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketTooFarOut})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanFiltersNonActiveMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketClosed})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanFiltersWideSpreadMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketWideSpread})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanFiltersNarrowSpreadMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketNarrowSpread})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanFiltersMarketsClosingTooSoon) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketTooSoon})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanReturnsEmptyForNoGoodMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketDeepLongshot, kMarketLowVolume,
                                       kMarketTooFarOut, kMarketClosed})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_TRUE(results.empty());
}

TEST_F(TickerScannerTest, ScanReturnsAtMostTopN) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk,
       make_markets_response({kMarketGoodA, kMarketGoodB, kMarketCrypto})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopTwo, kTestNow);

  EXPECT_EQ(results.size(), 2U);
}

TEST_F(TickerScannerTest, ScanRanksHigherVolumeFirst) {
  // kMarketGoodB has 400k volume vs kMarketGoodA's 200k; same category and
  // very similar spread/price. GoodB should score higher.
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].ticker, "KXCPI-B");
}

TEST_F(TickerScannerTest, ScanRanksFinancialsAboveCrypto) {
  // kMarketGoodA (Financials, 200k) vs kMarketCrypto (Crypto, 100k).
  // Category bonus alone (1.0 vs 0.7) plus volume should put Financials first.
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketCrypto, kMarketGoodA})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanPopulatesMarketScoreFields) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue({kHttpOk, make_markets_response({kMarketGoodA})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  const auto &market_score = results[0];
  EXPECT_EQ(market_score.ticker, "KXFED-A");
  EXPECT_EQ(market_score.title, "Fed holds at 5.25%?");
  EXPECT_EQ(market_score.category, "Financials");
  EXPECT_EQ(market_score.mid_price_cents, 50);
  EXPECT_EQ(market_score.spread_cents, 4);
  EXPECT_DOUBLE_EQ(market_score.volume_usd, kHighVolume);
  EXPECT_GT(market_score.days_to_close, 4.0);
  EXPECT_LT(market_score.days_to_close, 6.0);
  EXPECT_GT(market_score.score, 0.0);
  EXPECT_LE(market_score.score, 1.0);
}

TEST_F(TickerScannerTest, ScanAllFilteredReturnsEmpty) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue({kHttpOk, R"({"markets":[],"cursor":""})"});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_TRUE(results.empty());
}

TEST_F(TickerScannerTest, ScanRespectsScannerConfigOverrides) {
  // Raise min_volume to 150k — kMarketCrypto (100k) should be filtered out
  // but kMarketGoodA (200k) and kMarketGoodB (400k) pass.
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk,
       make_markets_response({kMarketGoodA, kMarketGoodB, kMarketCrypto})});

  kalshi::ScannerConfig config;
  config.min_volume_usd = kCustomMinVolumeUsd;
  kalshi::TickerScanner scanner{client, config};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U);
  for (const auto &result : results) {
    EXPECT_GE(result.volume_usd, 150000.0);
  }
}

TEST_F(TickerScannerTest, ScanResultsAreSortedByScoreDescending) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk,
       make_markets_response({kMarketGoodA, kMarketGoodB, kMarketCrypto})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_GE(results.size(), 2U);
  for (std::size_t idx = 1U; idx < results.size(); ++idx) {
    EXPECT_GE(results[idx - 1U].score, results[idx].score);
  }
}
