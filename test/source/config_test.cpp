#include "config.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path write_temp_config(const nlohmann::json &content) {
  const auto path =
      std::filesystem::temp_directory_path() / "kalshi_config_test.json";
  std::ofstream file{path};
  if (!file) {
    throw std::runtime_error{"Failed to create temp config file"};
  }
  file << content.dump();
  return path;
}

nlohmann::json minimal_config_json() {
  return {{"api_key", "test-api-key"},
          {"private_key_path", "/path/to/key.pem"},
          {"target_tickers", {"TICK-A"}}};
}

} // namespace

TEST(ConfigTest, LoadsAllFields) {
  constexpr int kSpreadCents = 6;
  constexpr double kSkew = 0.1;
  constexpr int kRepriceCents = 2;
  constexpr int kQuoteSize = 5;
  constexpr int kMaxPosition = 50;
  constexpr int kMaxOpenOrders = 2;
  constexpr int kMaxOrderSize = 10;
  constexpr double kDailyLossLimit = -100.0;

  const nlohmann::json config_json = {
      {"api_key", "my-key"},
      {"private_key_path", "/keys/prod.pem"},
      {"base_url", "https://demo-api.kalshi.co"},
      {"ws_url", "wss://demo-api.kalshi.co/trade-api/ws/v2"},
      {"target_tickers", {"TICK-A", "TICK-B"}},
      {"quoter",
       {{"target_spread_cents", kSpreadCents},
        {"skew_per_contract_cents", kSkew},
        {"reprice_threshold_cents", kRepriceCents},
        {"quote_size", kQuoteSize}}},
      {"risk",
       {{"max_position_per_market", kMaxPosition},
        {"max_open_orders_per_market", kMaxOpenOrders},
        {"max_order_size", kMaxOrderSize},
        {"daily_loss_limit", kDailyLossLimit}}}};

  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.api_key, "my-key");
  EXPECT_EQ(config.private_key_path, "/keys/prod.pem");
  EXPECT_EQ(config.base_url, "https://demo-api.kalshi.co");
  EXPECT_EQ(config.ws_url, "wss://demo-api.kalshi.co/trade-api/ws/v2");
  ASSERT_EQ(config.target_tickers.size(), 2U);
  EXPECT_EQ(config.target_tickers.at(0), "TICK-A");
  EXPECT_EQ(config.target_tickers.at(1), "TICK-B");
  EXPECT_EQ(config.quoter.target_spread_cents, kSpreadCents);
  EXPECT_DOUBLE_EQ(config.quoter.skew_per_contract_cents, kSkew);
  EXPECT_EQ(config.quoter.reprice_threshold_cents, kRepriceCents);
  EXPECT_EQ(config.quoter.quote_size, kQuoteSize);
  EXPECT_EQ(config.risk.max_position_per_market, kMaxPosition);
  EXPECT_EQ(config.risk.max_open_orders_per_market, kMaxOpenOrders);
  EXPECT_EQ(config.risk.max_order_size, kMaxOrderSize);
  EXPECT_DOUBLE_EQ(config.risk.daily_loss_limit, kDailyLossLimit);
}

TEST(ConfigTest, DefaultsAppliedWhenOptionalSectionsAbsent) {
  const auto path = write_temp_config(minimal_config_json());
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.base_url, "https://trading-api.kalshi.com/trade-api/v2");
  EXPECT_EQ(config.ws_url, "wss://trading-api.kalshi.com/trade-api/ws/v2");
  EXPECT_EQ(config.quoter.target_spread_cents,
            kalshi::QuoterConfig::kDefaultTargetSpreadCents);
  EXPECT_DOUBLE_EQ(config.quoter.skew_per_contract_cents,
                   kalshi::QuoterConfig::kDefaultSkewPerContractCents);
  EXPECT_EQ(config.quoter.reprice_threshold_cents,
            kalshi::QuoterConfig::kDefaultRepriceThresholdCents);
  EXPECT_EQ(config.quoter.quote_size, kalshi::QuoterConfig::kDefaultQuoteSize);
  EXPECT_EQ(config.risk.max_position_per_market,
            kalshi::RiskLimits::kDefaultMaxPosition);
  EXPECT_EQ(config.risk.max_open_orders_per_market,
            kalshi::RiskLimits::kDefaultMaxOpenOrders);
  EXPECT_EQ(config.risk.max_order_size,
            kalshi::RiskLimits::kDefaultMaxOrderSize);
  EXPECT_DOUBLE_EQ(config.risk.daily_loss_limit,
                   kalshi::RiskLimits::kDefaultDailyLossLimit);
}

TEST(ConfigTest, ThrowsOnMissingApiKey) {
  const nlohmann::json config_json = {{"private_key_path", "/key.pem"},
                                      {"target_tickers", {"TICK-A"}}};
  const auto path = write_temp_config(config_json);
  EXPECT_THROW(kalshi::load_config(path), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(ConfigTest, ThrowsOnMissingPrivateKeyPath) {
  const nlohmann::json config_json = {{"api_key", "key"},
                                      {"target_tickers", {"TICK-A"}}};
  const auto path = write_temp_config(config_json);
  EXPECT_THROW(kalshi::load_config(path), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(ConfigTest, ThrowsOnMissingTargetTickers) {
  const nlohmann::json config_json = {{"api_key", "key"},
                                      {"private_key_path", "/key.pem"}};
  const auto path = write_temp_config(config_json);
  EXPECT_THROW(kalshi::load_config(path), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(ConfigTest, ThrowsOnEmptyTargetTickers) {
  const nlohmann::json config_json = {
      {"api_key", "key"},
      {"private_key_path", "/key.pem"},
      {"target_tickers", nlohmann::json::array()}};
  const auto path = write_temp_config(config_json);
  EXPECT_THROW(kalshi::load_config(path), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(ConfigTest, ThrowsOnNonexistentFile) {
  EXPECT_THROW(kalshi::load_config("/nonexistent/path/config.json"),
               std::runtime_error);
}
