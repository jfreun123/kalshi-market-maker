#include "app_modes.hpp"

#include "fake_transport.hpp"
#include "logger.hpp"
#include "order_manager.hpp"
#include "risk_manager.hpp"

#include <gtest/gtest.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace {

constexpr int kHttpOk = 200;
const std::string kTicker = "KXTEST";
const std::string kBaseUrl = "https://demo-api.kalshi.co/trade-api/v2";

std::string generate_rsa_pem() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  EVP_PKEY *pkey = EVP_RSA_gen(2048U);
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  char *pem_data = nullptr;
  long pem_len = BIO_get_mem_data(bio, &pem_data); // NOLINT(runtime/int)
  std::string pem(pem_data, static_cast<std::size_t>(pem_len));
  BIO_free(bio);
  EVP_PKEY_free(pkey);
  return pem;
}

std::string
    kPemPrivateKey; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

} // namespace

class AppModesTest : public ::testing::Test {
public:
  static void SetUpTestSuite() { kPemPrivateKey = generate_rsa_pem(); }
};

TEST_F(AppModesTest, ReconcileInSyncReturnsTrueAndKeepsQuoting) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"market_positions":[],"cursor":""})"});
  kalshi::RestClient rest{kalshi::Auth{"key", kPemPrivateKey},
                          std::move(transport), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto log = kalshi::get_logger();

  EXPECT_TRUE(kalshi::reconcile_against_exchange(rest, order_mgr, {kTicker},
                                                 &risk_mgr, log));
  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kModelDiverge));
}

TEST_F(AppModesTest, ReconcileDriftHaltsQuoting) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"market_positions":[{"ticker":"KXTEST","position_fp":"7.00",)"
       R"("realized_pnl_dollars":"0","market_exposure_dollars":"0",)"
       R"("resting_orders_count":0}],"cursor":""})"});
  kalshi::RestClient rest{kalshi::Auth{"key", kPemPrivateKey},
                          std::move(transport), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto log = kalshi::get_logger();

  EXPECT_FALSE(kalshi::reconcile_against_exchange(rest, order_mgr, {kTicker},
                                                  &risk_mgr, log));
  EXPECT_TRUE(risk_mgr.is_set(kalshi::Constraint::kModelDiverge));
}

TEST_F(AppModesTest, FlattenAllPositionsNoOpWhenFlat) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"market_positions":[],"cursor":""})"});
  kalshi::RestClient rest{kalshi::Auth{"key", kPemPrivateKey},
                          std::move(transport), kBaseUrl};
  auto log = kalshi::get_logger();

  EXPECT_EQ(kalshi::flatten_all_positions(rest, log, nullptr), 0);
}

TEST_F(AppModesTest, ScanTopTickersEmptyWhenNoMarketsQualify) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"cursor":"","markets":[]})"});
  kalshi::RestClient rest{kalshi::Auth{"key", kPemPrivateKey},
                          std::move(transport), kBaseUrl};
  auto log = kalshi::get_logger();
  const kalshi::AppConfig app_config;

  const auto tickers = kalshi::scan_top_tickers(rest, app_config, log);

  EXPECT_TRUE(tickers.empty());
}

TEST(FvReplayModeTest, ReplaysCaptureAndPrintsScores) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("fv_replay_test_" + std::to_string(::getpid()) + ".jsonl");
  {
    std::ofstream fixture{path};
    fixture
        << R"({"type":"orderbook_snapshot","msg":{"market_ticker":"REPLAY-TICK",)"
        << R"("yes_dollars_fp":[["0.6000","10.00"]],)"
        << R"("no_dollars_fp":[["0.3600","10.00"]]}})" << "\n"
        << R"({"type":"trade","msg":{"trade_id":"t1",)"
        << R"("market_ticker":"REPLAY-TICK","yes_price_dollars":"0.6300",)"
        << R"("no_price_dollars":"0.3700","count_fp":"5.00",)"
        << R"("taker_outcome_side":"yes","taker_book_side":"bid",)"
        << R"("ts_ms":1783300000000}})" << "\n";
  }
  std::ostringstream table;
  auto log = kalshi::get_logger();
  const int result = kalshi::run_fv_replay_mode(path, table, log);
  std::filesystem::remove(path);

  ASSERT_EQ(result, 0);
  EXPECT_NE(table.str().find("micro"), std::string::npos);
  EXPECT_NE(table.str().find("clearing(flat)"), std::string::npos);
}

TEST(FvReplayModeTest, MissingCaptureFileFails) {
  std::ostringstream table;
  auto log = kalshi::get_logger();
  EXPECT_EQ(kalshi::run_fv_replay_mode(
                std::filesystem::path{"/nonexistent/nope.jsonl"}, table, log),
            1);
}
