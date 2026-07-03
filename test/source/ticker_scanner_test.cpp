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
constexpr int kWideMinSpread = 1;
constexpr int kWideMaxSpread = 20;
constexpr int kScanTopTwo = 2;
constexpr double kCustomMinVolumeUsd = 150000.0;

// ---- Market JSON building blocks ----

// Good market: mid=50c, spread=4c, 5 days out, high volume, Financials
constexpr std::string_view kMarketGoodA =
    R"({"ticker":"KXFED-A","title":"Fed holds at 5.25%?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

// Good market: mid=49.5c, spread=5c, 4 days out, higher volume, Financials
constexpr std::string_view kMarketGoodB =
    R"({"ticker":"KXCPI-B","title":"CPI above 3%?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4700","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"400000.00","close_time":"2026-06-25T00:00:00Z"})";

// Crypto market: mid=50c, spread=4c, 5 days out, mid volume
constexpr std::string_view kMarketCrypto =
    R"({"ticker":"KXBTCD-C","title":"Bitcoin above $70k?","category":"Crypto",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"100000.00","close_time":"2026-06-26T00:00:00Z"})";

// Crypto market with same volume/spread/mid as kMarketGoodA (Financials).
// Used to verify category does not bias the score.
constexpr std::string_view kMarketCryptoEqualVol =
    R"({"ticker":"KXBTCH-P","title":"BTC event?","category":"Crypto",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

constexpr std::string_view kMarketWideBand =
    R"({"ticker":"KXWIDEBAND-W","title":"Wide band market?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4200","yes_ask_dollars":"0.5700",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: price too low (mid=7.5c — deep longshot)
constexpr std::string_view kMarketDeepLongshot =
    R"({"ticker":"KXLONG-D","title":"Longshot event?","category":"Other",)"
    R"("status":"active","yes_bid_dollars":"0.0500","yes_ask_dollars":"0.1000",)"
    R"("volume_24h_fp":"500000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: price too high (mid=92.5c)
constexpr std::string_view kMarketDeepFavorite =
    R"({"ticker":"KXFAV-E","title":"Near-certain event?","category":"Other",)"
    R"("status":"active","yes_bid_dollars":"0.9000","yes_ask_dollars":"0.9500",)"
    R"("volume_24h_fp":"500000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: volume too low
constexpr std::string_view kMarketLowVolume =
    R"({"ticker":"KXLOW-F","title":"Illiquid market?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"500.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: closes in 96 days (past max_days_to_close=90)
constexpr std::string_view kMarketTooFarOut =
    R"({"ticker":"KXFAR-G","title":"Far future event?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-09-25T00:00:00Z"})";

// Mid-term market: closes in 60 days — should pass max_days_to_close=90 filter.
constexpr std::string_view kMarketMediumTerm =
    R"({"ticker":"KXMED-M","title":"Medium-term event?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-08-20T00:00:00Z"})";

// High-probability market: mid=75c, spread=4c, same volume/category as
// kMarketGoodA. Used to verify the scorer does not penalize markets away from
// 50c.
constexpr std::string_view kMarketHighProbability =
    R"({"ticker":"KXHIGH-P","title":"High probability event?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.7300","yes_ask_dollars":"0.7700",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: not active
constexpr std::string_view kMarketClosed =
    R"({"ticker":"KXCLS-H","title":"Already closed?","category":"Financials",)"
    R"("status":"closed","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-22T00:00:00Z"})";

// Filtered: spread too wide (>10c)
constexpr std::string_view kMarketWideSpread =
    R"({"ticker":"KXWIDE-I","title":"Wide spread market?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4000","yes_ask_dollars":"0.6000",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: spread too narrow (<3c) — indicates locked/crossed book
constexpr std::string_view kMarketNarrowSpread =
    R"({"ticker":"KXNARR-J","title":"Narrow spread market?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4900","yes_ask_dollars":"0.5000",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

// Filtered: closes in 0.3 days (too soon)
constexpr std::string_view kMarketTooSoon =
    R"({"ticker":"KXSOON-K","title":"Closing today?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-21T07:00:00Z"})";

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

std::string make_incentives_response(
    std::initializer_list<std::pair<std::string_view, long long>> pools) {
  std::string body = R"({"incentive_programs":[)";
  bool first = true;
  for (const auto &[ticker, reward] : pools) {
    if (!first) {
      body += ',';
    }
    body += R"({"market_ticker":")";
    body += ticker;
    body += R"(","period_reward":)";
    body += std::to_string(reward);
    body += R"(,"target_size_fp":"1000.00","discount_factor_bps":5000})";
    first = false;
  }
  body += R"(],"next_cursor":""})";
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

TEST_F(TickerScannerTest, SpreadScorePeaksAtConfiguredBandMidpoint) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketWideBand})});

  kalshi::ScannerConfig config;
  config.min_spread_cents = kWideMinSpread;
  config.max_spread_cents = kWideMaxSpread;
  kalshi::TickerScanner scanner{client, config};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results.front().ticker, "KXWIDEBAND-W")
      << "spread 15 sits nearer the [1,20] band midpoint (10.5) than spread 4 "
         "and must outscore it at equal volume";
}

TEST_F(TickerScannerTest, IncentivizedMarketOutranksEquivalentPeer) {
  auto [client, transport] = make_client_with_transport();
  constexpr long long kPoolRewardCentiCents = 650000; // $65
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketCryptoEqualVol})});
  transport->enqueue({kHttpOk, make_incentives_response(
                                   {{"KXBTCH-P", kPoolRewardCentiCents}})});

  kalshi::TickerScanner scanner{client};
  const auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].ticker, "KXBTCH-P");
  EXPECT_GT(results[0].incentive_reward_dollars, 0.0);
  EXPECT_EQ(results[1].ticker, "KXFED-A");
  EXPECT_DOUBLE_EQ(results[1].incentive_reward_dollars, 0.0);
  EXPECT_GT(results[0].score, results[1].score);
}

TEST_F(TickerScannerTest, NoIncentivePoolsLeaveRewardZero) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue({kHttpOk, make_markets_response({kMarketGoodA})});
  transport->enqueue({kHttpOk, make_incentives_response({})});

  kalshi::TickerScanner scanner{client};
  const auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_DOUBLE_EQ(results[0].incentive_reward_dollars, 0.0);
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
  // kMarketTooFarOut closes in 96 days, past the max_days_to_close=90
  // threshold.
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

TEST_F(TickerScannerTest, ScanRanksHigherVolumeFirstAcrossCategories) {
  // kMarketGoodA (Financials, 200k) should rank above kMarketCrypto (Crypto,
  // 100k) purely on volume — category is not a scoring factor.
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketCrypto, kMarketGoodA})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, ScanDoesNotBiasForFinancials) {
  // A Crypto market and a Financials market with identical volume, spread, and
  // price should receive equal scores. Category does not predict fill rate or
  // MM edge and must not influence ranking.
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketCryptoEqualVol})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 2U);
  EXPECT_NEAR(results[0].score, results[1].score, 1e-9);
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
  EXPECT_DOUBLE_EQ(market_score.volume_24h, kHighVolume);
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
  config.min_volume_24h = kCustomMinVolumeUsd;
  kalshi::TickerScanner scanner{client, config};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U);
  for (const auto &result : results) {
    EXPECT_GE(result.volume_24h, 150000.0);
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

TEST_F(TickerScannerTest, ScanAcceptsMediumTermMarkets) {
  // max_days_to_close=90 admits markets up to 90 days out; this one is 60 days.
  auto [client, transport] = make_client_with_transport();
  transport->enqueue({kHttpOk, make_markets_response({kMarketMediumTerm})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].ticker, "KXMED-M");
}

TEST_F(TickerScannerTest, ScanSearchesAllEventSeries) {
  // When event_series is set, the scanner fetches markets for each series
  // separately and merges the results before filtering and scoring.
  auto [client, transport] = make_client_with_transport();
  transport->enqueue({kHttpOk, make_markets_response({kMarketGoodA})});
  transport->enqueue({kHttpOk, make_markets_response({kMarketCrypto})});

  kalshi::ScannerConfig config;
  config.event_series = {"KXCPI-26AUG", "KXCPI-26SEP"};
  kalshi::TickerScanner scanner{client, config};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U);
}

TEST_F(TickerScannerTest, ScanDoesNotPenalizeHighProbabilityMarkets) {
  // A market at 75c mid and a market at 50c mid with identical volume, spread,
  // and category should receive equal scores. A market maker earns the same
  // spread regardless of the market's implied probability.
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketHighProbability})});

  kalshi::TickerScanner scanner{client};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 2U);
  EXPECT_NEAR(results[0].score, results[1].score, 1e-9);
}
