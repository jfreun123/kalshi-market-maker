#include "app_modes.hpp"

#include "fake_transport.hpp"
#include "logger.hpp"
#include "order_manager.hpp"
#include "paper_transport.hpp"
#include "risk_manager.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <format>
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

TEST_F(AppModesTest, FlattenScopedToTickersSkipsOtherPositions) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk, R"({"market_positions":[)"
                R"({"ticker":"KXMINE","position_fp":"5.00",)"
                R"("realized_pnl_dollars":"0","market_exposure_dollars":"0",)"
                R"("resting_orders_count":0},)"
                R"({"ticker":"KXOTHER","position_fp":"7.00",)"
                R"("realized_pnl_dollars":"0","market_exposure_dollars":"0",)"
                R"("resting_orders_count":0}],"cursor":""})"});
  transport_raw->enqueue(
      {kHttpOk,
       R"({"order_id":"flat-1","fill_count":"5.00","remaining_count":"0.00",)"
       R"("ts_ms":1718000000000})"});
  kalshi::RestClient rest{kalshi::Auth{"key", kPemPrivateKey},
                          std::move(transport), kBaseUrl};
  auto log = kalshi::get_logger();
  const std::vector<std::string> mine = {"KXMINE"};

  EXPECT_EQ(kalshi::flatten_all_positions(rest, log, nullptr, &mine), 1);
}

namespace {

std::string paper_order_body(const std::string &ticker, kalshi::Side side,
                             int price_cents, int quantity) {
  const int yes_price_cents =
      (side == kalshi::Side::Yes) ? price_cents : (100 - price_cents);
  nlohmann::json body;
  body["ticker"] = ticker;
  body["side"] = (side == kalshi::Side::Yes) ? "bid" : "ask";
  body["price"] =
      std::format("{:.4f}", static_cast<double>(yes_price_cents) / 100.0);
  body["count"] = std::format("{:.2f}", static_cast<double>(quantity));
  body["time_in_force"] = "good_till_canceled";
  return body.dump();
}

kalshi::PublicTrade sim_print(const std::string &ticker, int yes_price,
                              kalshi::Side taker, int contracts) {
  kalshi::PublicTrade print;
  print.trade_id = "print-1";
  print.market_ticker = ticker;
  print.yes_price_cents = yes_price;
  print.quantity = kalshi::Quantity::from_contracts(contracts);
  print.taker_side = taker;
  print.timestamp = std::chrono::system_clock::now();
  return print;
}

} // namespace

TEST_F(AppModesTest, PrintThroughFillsRestingAsk) {
  kalshi::PaperTransport paper;
  (void)paper.post("/portfolio/orders", {},
                   paper_order_body("KXBT", kalshi::Side::No, 46, 10));

  constexpr int kAskYes = 54;
  const int filled = kalshi::simulate_maker_fills(
      paper, nullptr, sim_print("KXBT", kAskYes + 2, kalshi::Side::Yes, 5));

  EXPECT_EQ(filled, 1);
  ASSERT_EQ(paper.fills().size(), 1U);
  EXPECT_EQ(paper.open_orders().front().filled_quantity,
            kalshi::Quantity::from_contracts(5));
}

TEST_F(AppModesTest, AtLevelPrintDoesNotFill) {
  kalshi::PaperTransport paper;
  (void)paper.post("/portfolio/orders", {},
                   paper_order_body("KXBT", kalshi::Side::No, 46, 10));

  constexpr int kAskYes = 54;
  const int filled = kalshi::simulate_maker_fills(
      paper, nullptr, sim_print("KXBT", kAskYes, kalshi::Side::Yes, 5));

  EXPECT_EQ(filled, 0) << "conservative model: at-level prints are queue-"
                          "dependent, never simulated as fills";
}

TEST_F(AppModesTest, SellPrintThroughFillsRestingBid) {
  kalshi::PaperTransport paper;
  (void)paper.post("/portfolio/orders", {},
                   paper_order_body("KXBT", kalshi::Side::Yes, 50, 10));

  const int filled = kalshi::simulate_maker_fills(
      paper, nullptr, sim_print("KXBT", 48, kalshi::Side::No, 3));

  EXPECT_EQ(filled, 1);
  EXPECT_EQ(paper.open_orders().front().filled_quantity,
            kalshi::Quantity::from_contracts(3));
}

TEST_F(AppModesTest, WrongSideOrTickerPrintsDoNotFill) {
  kalshi::PaperTransport paper;
  (void)paper.post("/portfolio/orders", {},
                   paper_order_body("KXBT", kalshi::Side::Yes, 50, 10));

  EXPECT_EQ(kalshi::simulate_maker_fills(
                paper, nullptr, sim_print("KXBT", 56, kalshi::Side::Yes, 5)),
            0)
      << "taker buying does not fill our bid";
  EXPECT_EQ(kalshi::simulate_maker_fills(
                paper, nullptr, sim_print("KXOTHER", 40, kalshi::Side::No, 5)),
            0)
      << "other market's print";
}

TEST_F(AppModesTest, BacktestModeQuotesAndSimulatesPrintThroughFills) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("backtest_test_" + std::to_string(::getpid()) + ".jsonl");
  {
    std::ofstream fixture{path};
    fixture
        << R"({"type":"orderbook_snapshot","msg":{"market_ticker":"BT-TICK",)"
        << R"("yes_dollars_fp":[["0.5100","100.00"]],)"
        << R"("no_dollars_fp":[["0.4700","100.00"]]}})" << "\n"
        << R"({"type":"trade","msg":{"trade_id":"t1",)"
        << R"("market_ticker":"BT-TICK","yes_price_dollars":"0.5900",)"
        << R"("no_price_dollars":"0.4100","count_fp":"5.00",)"
        << R"("taker_outcome_side":"yes","taker_book_side":"bid",)"
        << R"("ts_ms":1783300000000}})" << "\n";
  }
  std::ostringstream out;
  auto log = kalshi::get_logger();
  const kalshi::AppConfig app_config;
  const int result = kalshi::run_backtest_mode(path, app_config, out, log);
  std::filesystem::remove(path);

  ASSERT_EQ(result, 0);
  EXPECT_NE(out.str().find("sim_fills=1"), std::string::npos) << out.str();
  EXPECT_NE(out.str().find("BT-TICK"), std::string::npos);
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
