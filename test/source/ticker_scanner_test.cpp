#include "exchange/ticker_scanner.hpp"
#include "fake_transport.hpp"

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

// Filtered: ticker-dated 2026-06-19, two days before kTestNow — the event
// already happened even though close_time (settlement window) is weeks out
constexpr std::string_view kMarketExpiredEventDate =
    R"({"ticker":"KXOLD-26JUN19FOO-BAR","title":"Already-played event?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-07-19T00:00:00Z"})";

// Kept: ticker-dated exactly kTestNow's date (2026-06-21) — event is today
constexpr std::string_view kMarketTodayEventDate =
    R"({"ticker":"KXNOW-26JUN211830FOO-BAR","title":"Event happening today?","category":"Financials",)"
    R"("status":"active","yes_bid_dollars":"0.4800","yes_ask_dollars":"0.5200",)"
    R"("volume_24h_fp":"200000.00","close_time":"2026-06-26T00:00:00Z"})";

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

TEST_F(TickerScannerTest, StaleMarketDroppedByLivenessFilter) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T21:00:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"t-old","yes_price_dollars":"0.5000"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:45:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"t-fresh","yes_price_dollars":"0.5000"}]})"});

  kalshi::ScannerConfig staleness_only;
  staleness_only.min_trades_per_hour = 0;
  staleness_only.min_spread_cents = 0;
  staleness_only.min_trade_price_range_cents = 0;
  kalshi::TickerScanner scanner{client, staleness_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U)
      << "a market whose last trade is 3h old must be dropped";
  EXPECT_EQ(results.front().ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, LivenessFilterDisabledKeepsStaleMarkets) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});

  kalshi::ScannerConfig no_admission;
  no_admission.max_stale_trade_minutes = 0;
  no_admission.min_trades_per_hour = 0;
  no_admission.min_spread_cents = 0;
  no_admission.min_trade_price_range_cents = 0;
  kalshi::TickerScanner scanner{client, no_admission};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U);
}

TEST_F(TickerScannerTest, SparseFlowMarketDropped) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.4800"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b2","yes_price_dollars":"0.5100"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.4800"},)"
       R"({"created_time":"2026-06-20T21:00:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.5100"}]})"});

  kalshi::ScannerConfig flow_only;
  flow_only.max_stale_trade_minutes = 0;
  flow_only.min_spread_cents = 0;
  flow_only.min_trades_per_hour = 2;
  flow_only.min_trade_price_range_cents = 0;
  kalshi::TickerScanner scanner{client, flow_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U)
      << "a market with fewer than min_trades_per_hour recent trades must be "
         "dropped";
  EXPECT_EQ(results.front().ticker, "KXCPI-B");
}

TEST_F(TickerScannerTest, NeverTradedMarketDroppedWhenFlowFilterActive) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue({kHttpOk, R"({"cursor":"","trades":[]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.5000"}]})"});

  kalshi::ScannerConfig flow_only;
  flow_only.max_stale_trade_minutes = 0;
  flow_only.min_spread_cents = 0;
  flow_only.min_trades_per_hour = 1;
  flow_only.min_trade_price_range_cents = 0;
  kalshi::TickerScanner scanner{client, flow_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U)
      << "a parsed-but-empty trade tape is a definitive drop, not fail-open";
  EXPECT_EQ(results.front().ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, TightLiveBookDropped) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue({kHttpOk,
                      R"({"orderbook_fp":{"yes_dollars":[["0.2000","500.00"]],)"
                      R"("no_dollars":[["0.7900","400.00"]]}})"});
  transport->enqueue({kHttpOk,
                      R"({"orderbook_fp":{"yes_dollars":[["0.2000","500.00"]],)"
                      R"("no_dollars":[["0.7400","400.00"]]}})"});

  kalshi::ScannerConfig book_only;
  book_only.max_stale_trade_minutes = 0;
  book_only.min_trades_per_hour = 0;
  book_only.min_trade_price_range_cents = 0;
  kalshi::TickerScanner scanner{client, book_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U)
      << "a live book tighter than min_spread_cents must be dropped";
  EXPECT_EQ(results.front().ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, OneSidedLiveBookAdmitted) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue({kHttpOk,
                      R"({"orderbook_fp":{"yes_dollars":[["0.2000","500.00"]],)"
                      R"("no_dollars":[]}})"});
  transport->enqueue({kHttpOk,
                      R"({"orderbook_fp":{"yes_dollars":[["0.2000","500.00"]],)"
                      R"("no_dollars":[["0.7400","400.00"]]}})"});

  kalshi::ScannerConfig book_only;
  book_only.max_stale_trade_minutes = 0;
  book_only.min_trades_per_hour = 0;
  book_only.min_trade_price_range_cents = 0;
  kalshi::TickerScanner scanner{client, book_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U)
      << "an empty book side means room to quote, not a tight book";
}

TEST_F(TickerScannerTest, SparseButMovingTapeAdmitted) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.5500"},)"
       R"({"created_time":"2026-06-20T22:10:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b2","yes_price_dollars":"0.5200"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.5500"},)"
       R"({"created_time":"2026-06-20T22:10:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.5200"}]})"});

  kalshi::ScannerConfig tape_only;
  tape_only.max_stale_trade_minutes = 0;
  tape_only.min_trades_per_hour = 0;
  tape_only.min_spread_cents = 0;
  tape_only.min_trade_price_range_cents = 2;
  constexpr int kThreeHourLookback = 180;
  tape_only.tape_range_lookback_minutes = kThreeHourLookback;
  kalshi::TickerScanner scanner{client, tape_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U)
      << "pre-game markets print sparsely; price discovery over the lookback "
         "window counts even when the last hour is quiet";
}

TEST_F(TickerScannerTest, OneWayTapeDroppedByTwoSidedGate) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.5500",)"
       R"("count_fp":"10.00","taker_outcome_side":"yes"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b2","yes_price_dollars":"0.5200",)"
       R"("count_fp":"12.00","taker_outcome_side":"yes"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.5500",)"
       R"("count_fp":"10.00","taker_outcome_side":"yes"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.5200",)"
       R"("count_fp":"12.00","taker_outcome_side":"yes"}]})"});

  kalshi::ScannerConfig flow_only;
  flow_only.max_stale_trade_minutes = 0;
  flow_only.min_trades_per_hour = 0;
  flow_only.min_spread_cents = 0;
  flow_only.min_trade_price_range_cents = 0;
  flow_only.min_minority_flow_ratio = 0.2;
  kalshi::TickerScanner scanner{client, flow_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 0U) << "every recent print is the same taker side "
                                   "— round trips are impossible";
}

TEST_F(TickerScannerTest, BalancedTapePassesTwoSidedGate) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.5500",)"
       R"("count_fp":"10.00","taker_outcome_side":"yes"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b2","yes_price_dollars":"0.5200",)"
       R"("count_fp":"6.00","taker_outcome_side":"no"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.5500",)"
       R"("count_fp":"10.00","taker_outcome_side":"yes"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.5200",)"
       R"("count_fp":"6.00","taker_outcome_side":"no"}]})"});

  kalshi::ScannerConfig flow_only;
  flow_only.max_stale_trade_minutes = 0;
  flow_only.min_trades_per_hour = 0;
  flow_only.min_spread_cents = 0;
  flow_only.min_trade_price_range_cents = 0;
  flow_only.min_minority_flow_ratio = 0.2;
  kalshi::TickerScanner scanner{client, flow_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U)
      << "minority side has 6/16 = 37.5% of recent volume";
}

TEST_F(TickerScannerTest, TwoSidedGateFallsBackToPrintCountsWithoutSizes) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.5500",)"
       R"("taker_outcome_side":"yes"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b2","yes_price_dollars":"0.5200",)"
       R"("taker_outcome_side":"no"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.5500",)"
       R"("taker_outcome_side":"yes"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.5200",)"
       R"("taker_outcome_side":"no"}]})"});

  kalshi::ScannerConfig flow_only;
  flow_only.max_stale_trade_minutes = 0;
  flow_only.min_trades_per_hour = 0;
  flow_only.min_spread_cents = 0;
  flow_only.min_trade_price_range_cents = 0;
  flow_only.min_minority_flow_ratio = 0.2;
  kalshi::TickerScanner scanner{client, flow_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U)
      << "no count_fp on the wire — one print per side passes on counts";
}

TEST_F(TickerScannerTest, ZeroRatioDisablesTwoSidedGate) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.5500",)"
       R"("count_fp":"10.00","taker_outcome_side":"yes"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b2","yes_price_dollars":"0.5200",)"
       R"("count_fp":"12.00","taker_outcome_side":"yes"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.5500",)"
       R"("count_fp":"10.00","taker_outcome_side":"yes"},)"
       R"({"created_time":"2026-06-20T23:30:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.5200",)"
       R"("count_fp":"12.00","taker_outcome_side":"yes"}]})"});

  kalshi::ScannerConfig flow_only;
  flow_only.max_stale_trade_minutes = 0;
  flow_only.min_trades_per_hour = 0;
  flow_only.min_spread_cents = 0;
  flow_only.min_trade_price_range_cents = 0;
  flow_only.min_minority_flow_ratio = 0.0;
  kalshi::TickerScanner scanner{client, flow_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U)
      << "gate off by default — one-way tape admitted";
}

TEST_F(TickerScannerTest, RevertingTapePassesReversionGate) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"candlesticks":[{"end_period_ts":1,"price":{"close_dollars":"0.5000"}},{"end_period_ts":2,"price":{"close_dollars":"0.5300"}},{"end_period_ts":3,"price":{"close_dollars":"0.5000"}},{"end_period_ts":4,"price":{"close_dollars":"0.5300"}}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"candlesticks":[{"end_period_ts":1,"price":{"close_dollars":"0.5000"}},{"end_period_ts":2,"price":{"close_dollars":"0.5300"}},{"end_period_ts":3,"price":{"close_dollars":"0.5000"}},{"end_period_ts":4,"price":{"close_dollars":"0.5300"}}]})"});

  kalshi::ScannerConfig reversion_only;
  reversion_only.max_stale_trade_minutes = 0;
  reversion_only.min_trades_per_hour = 0;
  reversion_only.min_spread_cents = 0;
  reversion_only.min_trade_price_range_cents = 0;
  reversion_only.min_reversion_kappa = 1.0;
  kalshi::TickerScanner scanner{client, reversion_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U)
      << "closes 50,53,50,53: K=9, z=3, K >= 1.0*z^2=9 — harvestable wiggle";
}

TEST_F(TickerScannerTest, TrendingTapeDroppedByReversionGate) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"candlesticks":[{"end_period_ts":1,"price":{"close_dollars":"0.5000"}},{"end_period_ts":2,"price":{"close_dollars":"0.5300"}},{"end_period_ts":3,"price":{"close_dollars":"0.5600"}},{"end_period_ts":4,"price":{"close_dollars":"0.5900"}}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"candlesticks":[{"end_period_ts":1,"price":{"close_dollars":"0.5000"}},{"end_period_ts":2,"price":{"close_dollars":"0.5300"}},{"end_period_ts":3,"price":{"close_dollars":"0.5600"}},{"end_period_ts":4,"price":{"close_dollars":"0.5900"}}]})"});

  kalshi::ScannerConfig reversion_only;
  reversion_only.max_stale_trade_minutes = 0;
  reversion_only.min_trades_per_hour = 0;
  reversion_only.min_spread_cents = 0;
  reversion_only.min_trade_price_range_cents = 0;
  reversion_only.min_reversion_kappa = 1.0;
  kalshi::TickerScanner scanner{client, reversion_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 0U)
      << "closes 50,53,56,59: K=9 < z^2=81 — pure trend, the z^2 loss case";
}

TEST_F(TickerScannerTest, ZeroKappaDisablesReversionGate) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});

  kalshi::ScannerConfig gates_off;
  gates_off.max_stale_trade_minutes = 0;
  gates_off.min_trades_per_hour = 0;
  gates_off.min_spread_cents = 0;
  gates_off.min_trade_price_range_cents = 0;
  gates_off.min_reversion_kappa = 0.0;
  kalshi::TickerScanner scanner{client, gates_off};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 2U) << "gate off — no candle probe, no drops";
}

TEST_F(TickerScannerTest, ShortLookbackDropsSparseTape) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.5500"},)"
       R"({"created_time":"2026-06-20T22:10:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b2","yes_price_dollars":"0.5200"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.5500"},)"
       R"({"created_time":"2026-06-20T22:10:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.5200"}]})"});

  kalshi::ScannerConfig tape_only;
  tape_only.max_stale_trade_minutes = 0;
  tape_only.min_trades_per_hour = 0;
  tape_only.min_spread_cents = 0;
  tape_only.min_trade_price_range_cents = 2;
  constexpr int kShortLookback = 60;
  tape_only.tape_range_lookback_minutes = kShortLookback;
  kalshi::TickerScanner scanner{client, tape_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(results.size(), 0U)
      << "with a short lookback the 100-minute-old print is invisible and "
         "the tape looks pinned";
}

TEST_F(TickerScannerTest, TickerDatedPastEventDropped) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketExpiredEventDate,
                                       kMarketTodayEventDate})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});

  kalshi::ScannerConfig no_admission;
  no_admission.max_stale_trade_minutes = 0;
  no_admission.min_trades_per_hour = 0;
  no_admission.min_spread_cents = 0;
  no_admission.min_trade_price_range_cents = 0;
  kalshi::TickerScanner scanner{client, no_admission};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 2U)
      << "a ticker dated before today is an already-decided event";
  for (const auto &result : results) {
    EXPECT_NE(result.ticker, "KXOLD-26JUN19FOO-BAR");
  }
}

TEST_F(TickerScannerTest, PinnedTapeDropped) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.2400"},)"
       R"({"created_time":"2026-06-20T23:40:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b2","yes_price_dollars":"0.2400"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.2400"},)"
       R"({"created_time":"2026-06-20T23:40:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.2600"}]})"});

  kalshi::ScannerConfig tape_only;
  tape_only.max_stale_trade_minutes = 0;
  tape_only.min_trades_per_hour = 0;
  tape_only.min_spread_cents = 0;
  tape_only.min_trade_price_range_cents = 2;
  kalshi::TickerScanner scanner{client, tape_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U)
      << "a tape pinned at one price is a determined market, not a live one";
  EXPECT_EQ(results.front().ticker, "KXFED-A");
}

TEST_F(TickerScannerTest, SingleRecentTradeFailsTapeRangeCheck) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  transport->enqueue({kHttpOk, R"({"incentive_programs":[],"cursor":""})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXCPI-B","trade_id":"b1","yes_price_dollars":"0.2400"}]})"});
  transport->enqueue(
      {kHttpOk,
       R"({"cursor":"","trades":[{"created_time":"2026-06-20T23:50:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a1","yes_price_dollars":"0.2400"},)"
       R"({"created_time":"2026-06-20T23:40:00Z",)"
       R"("ticker":"KXFED-A","trade_id":"a2","yes_price_dollars":"0.2700"}]})"});

  kalshi::ScannerConfig tape_only;
  tape_only.max_stale_trade_minutes = 0;
  tape_only.min_trades_per_hour = 0;
  tape_only.min_spread_cents = 0;
  tape_only.min_trade_price_range_cents = 2;
  kalshi::TickerScanner scanner{client, tape_only};
  auto results = scanner.scan(kScanTopN, kTestNow);

  ASSERT_EQ(results.size(), 1U)
      << "fewer than two recent trades cannot demonstrate price discovery";
  EXPECT_EQ(results.front().ticker, "KXFED-A");
}

namespace {

int count_markets_listing_requests(const FakeTransport &transport) {
  int matches = 0;
  for (const auto &request : transport.recorded_requests()) {
    if (request.url.find("/markets?") != std::string::npos) {
      ++matches;
    }
  }
  return matches;
}

} // namespace

TEST_F(TickerScannerTest, MarketListingCachedAcrossScansWithinTtl) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue({kHttpOk, make_markets_response({kMarketGoodA})});
  kalshi::TickerScanner scanner{client};

  const auto first = scanner.scan(kScanTopN, kTestNow);
  const int listing_requests = count_markets_listing_requests(*transport);
  const auto second =
      scanner.scan(kScanTopN, kTestNow + std::chrono::minutes{5});

  EXPECT_EQ(first.size(), 1U);
  EXPECT_EQ(second.size(), 1U);
  EXPECT_EQ(count_markets_listing_requests(*transport), listing_requests)
      << "listing reused from cache inside the TTL";

  transport->enqueue(
      {kHttpOk, make_markets_response({kMarketGoodA, kMarketGoodB})});
  constexpr auto kPastTtl = std::chrono::minutes{31};
  const auto third = scanner.scan(kScanTopN, kTestNow + kPastTtl);

  EXPECT_EQ(third.size(), 2U) << "TTL elapsed — listing refetched";
  EXPECT_GT(count_markets_listing_requests(*transport), listing_requests);
}

TEST_F(TickerScannerTest, MarketListingCacheDisabledByZeroTtl) {
  auto [client, transport] = make_client_with_transport();
  transport->enqueue({kHttpOk, make_markets_response({kMarketGoodA})});
  kalshi::ScannerConfig no_cache;
  no_cache.market_cache_minutes = 0;
  kalshi::TickerScanner scanner{client, no_cache};

  const auto first = scanner.scan(kScanTopN, kTestNow);
  const int listing_requests = count_markets_listing_requests(*transport);
  transport->enqueue({kHttpOk, make_markets_response({kMarketGoodA})});
  const auto second = scanner.scan(kScanTopN, kTestNow);

  EXPECT_EQ(first.size(), second.size());
  EXPECT_GT(count_markets_listing_requests(*transport), listing_requests);
}
