#include "app_modes.hpp"

#include "fake_transport.hpp"
#include "logger.hpp"
#include "order_manager.hpp"
#include "risk_manager.hpp"

#include <gtest/gtest.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
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
