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
  constexpr double kMaxTotalLoss = -250.0;
  constexpr int kMinQuotePrice = 12;
  constexpr int kMaxQuotePrice = 88;
  constexpr double kMaxDrawdown = 300.0;
  constexpr int kImbalanceSpread = 3;
  constexpr int kMinSpread = 5;
  constexpr bool kUseViewBased = true;
  constexpr double kViewBeta = 0.08;
  constexpr double kMakerFee = 0.07;
  constexpr int kFlowWindowSeconds = 120;
  constexpr double kFlowRatioThreshold = 1.5;
  constexpr int kFlowMinVolume = 10;

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
        {"quote_size", kQuoteSize},
        {"imbalance_spread_cents", kImbalanceSpread},
        {"min_spread_cents", kMinSpread},
        {"use_view_based_pricing", kUseViewBased},
        {"view_debias_beta", kViewBeta},
        {"maker_fee_rate", kMakerFee}}},
      {"flow",
       {{"window_seconds", kFlowWindowSeconds},
        {"imbalance_ratio_threshold", kFlowRatioThreshold},
        {"min_flow_volume", kFlowMinVolume}}},
      {"risk",
       {{"max_position_per_market", kMaxPosition},
        {"max_open_orders_per_market", kMaxOpenOrders},
        {"max_order_size", kMaxOrderSize},
        {"daily_loss_limit", kDailyLossLimit},
        {"max_total_loss_dollars", kMaxTotalLoss},
        {"min_quote_price_cents", kMinQuotePrice},
        {"max_quote_price_cents", kMaxQuotePrice},
        {"max_drawdown_dollars", kMaxDrawdown}}}};

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
  EXPECT_EQ(config.quoter.imbalance_spread_cents, kImbalanceSpread);
  EXPECT_EQ(config.quoter.min_spread_cents, kMinSpread);
  EXPECT_EQ(config.quoter.use_view_based_pricing, kUseViewBased);
  EXPECT_DOUBLE_EQ(config.quoter.view_debias_beta, kViewBeta);
  EXPECT_DOUBLE_EQ(config.quoter.maker_fee_rate, kMakerFee);
  EXPECT_EQ(config.flow.window_seconds, kFlowWindowSeconds);
  EXPECT_DOUBLE_EQ(config.flow.imbalance_ratio_threshold, kFlowRatioThreshold);
  EXPECT_EQ(config.flow.min_flow_volume, kFlowMinVolume);
  EXPECT_EQ(config.risk.max_position_per_market, kMaxPosition);
  EXPECT_EQ(config.risk.max_open_orders_per_market, kMaxOpenOrders);
  EXPECT_EQ(config.risk.max_order_size, kMaxOrderSize);
  EXPECT_DOUBLE_EQ(config.risk.daily_loss_limit, kDailyLossLimit);
  EXPECT_DOUBLE_EQ(config.risk.max_total_loss_dollars, kMaxTotalLoss);
  EXPECT_EQ(config.risk.min_quote_price_cents, kMinQuotePrice);
  EXPECT_EQ(config.risk.max_quote_price_cents, kMaxQuotePrice);
  EXPECT_DOUBLE_EQ(config.risk.max_drawdown_dollars, kMaxDrawdown);
}

TEST(ConfigTest, LoadsScannerSection) {
  constexpr int kMinPrice = 5;
  constexpr int kMaxPrice = 95;
  constexpr int kMinSpread = 1;
  constexpr int kMaxSpread = 20;
  constexpr double kMinVolume = 25.0;
  constexpr double kMinDays = 0.5;
  constexpr double kMaxDays = 120.0;

  nlohmann::json config_json = minimal_config_json();
  config_json["scanner"] = {
      {"min_price_cents", kMinPrice},   {"max_price_cents", kMaxPrice},
      {"min_spread_cents", kMinSpread}, {"max_spread_cents", kMaxSpread},
      {"min_volume_24h", kMinVolume},   {"min_days_to_close", kMinDays},
      {"max_days_to_close", kMaxDays}};

  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.scanner.min_price_cents, kMinPrice);
  EXPECT_EQ(config.scanner.max_price_cents, kMaxPrice);
  EXPECT_EQ(config.scanner.min_spread_cents, kMinSpread);
  EXPECT_EQ(config.scanner.max_spread_cents, kMaxSpread);
  EXPECT_DOUBLE_EQ(config.scanner.min_volume_24h, kMinVolume);
  EXPECT_DOUBLE_EQ(config.scanner.min_days_to_close, kMinDays);
  EXPECT_DOUBLE_EQ(config.scanner.max_days_to_close, kMaxDays);
}

TEST(ConfigTest, DefaultsAppliedWhenOptionalSectionsAbsent) {
  const auto path = write_temp_config(minimal_config_json());
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.base_url, "https://trading-api.kalshi.com/trade-api/v2");
  EXPECT_EQ(config.scanner.min_volume_24h,
            kalshi::ScannerConfig::kDefaultMinVolume24h);
  EXPECT_EQ(config.scanner.max_days_to_close,
            kalshi::ScannerConfig::kDefaultMaxDaysToClose);
  EXPECT_EQ(config.ws_url, "wss://trading-api.kalshi.com/trade-api/ws/v2");
  EXPECT_EQ(config.quoter.target_spread_cents,
            kalshi::QuoterConfig::kDefaultTargetSpreadCents);
  EXPECT_DOUBLE_EQ(config.quoter.skew_per_contract_cents,
                   kalshi::QuoterConfig::kDefaultSkewPerContractCents);
  EXPECT_EQ(config.quoter.reprice_threshold_cents,
            kalshi::QuoterConfig::kDefaultRepriceThresholdCents);
  EXPECT_EQ(config.quoter.quote_size, kalshi::QuoterConfig::kDefaultQuoteSize);
  EXPECT_EQ(config.quoter.imbalance_spread_cents,
            kalshi::QuoterConfig::kDefaultImbalanceSpreadCents);
  EXPECT_EQ(config.quoter.min_spread_cents,
            kalshi::QuoterConfig::kDefaultMinSpreadCents);
  EXPECT_FALSE(config.quoter.use_view_based_pricing);
  EXPECT_DOUBLE_EQ(config.quoter.view_debias_beta,
                   kalshi::ViewBasedModel::kDefaultBeta);
  EXPECT_DOUBLE_EQ(config.quoter.maker_fee_rate, 0.0);
  EXPECT_EQ(config.flow.window_seconds,
            kalshi::FlowImbalanceConfig::kDefaultWindowSeconds);
  EXPECT_DOUBLE_EQ(config.flow.imbalance_ratio_threshold,
                   kalshi::FlowImbalanceConfig::kDefaultRatioThreshold);
  EXPECT_EQ(config.flow.min_flow_volume,
            kalshi::FlowImbalanceConfig::kDefaultMinFlowVolume);
  EXPECT_EQ(config.risk.max_position_per_market,
            kalshi::RiskLimits::kDefaultMaxPosition);
  EXPECT_EQ(config.risk.max_open_orders_per_market,
            kalshi::RiskLimits::kDefaultMaxOpenOrders);
  EXPECT_EQ(config.risk.max_order_size,
            kalshi::RiskLimits::kDefaultMaxOrderSize);
  EXPECT_DOUBLE_EQ(config.risk.daily_loss_limit,
                   kalshi::RiskLimits::kDefaultDailyLossLimit);
  EXPECT_DOUBLE_EQ(config.risk.max_total_exposure_dollars,
                   kalshi::RiskLimits::kDefaultMaxTotalExposure);
  EXPECT_DOUBLE_EQ(config.risk.max_total_loss_dollars,
                   kalshi::RiskLimits::kDefaultMaxTotalLoss);
  EXPECT_EQ(config.risk.min_quote_price_cents,
            kalshi::RiskLimits::kDefaultMinQuotePrice);
  EXPECT_EQ(config.risk.max_quote_price_cents,
            kalshi::RiskLimits::kDefaultMaxQuotePrice);
  EXPECT_DOUBLE_EQ(config.risk.max_drawdown_dollars,
                   kalshi::RiskLimits::kDefaultMaxDrawdown);
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

TEST(ConfigTest, MissingTargetTickersFieldDefaultsToEmpty) {
  const nlohmann::json config_json = {{"api_key", "key"},
                                      {"private_key_path", "/key.pem"}};
  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);
  EXPECT_TRUE(config.target_tickers.empty());
}

TEST(ConfigTest, EmptyTargetTickersLoadsSuccessfully) {
  const nlohmann::json config_json = {
      {"api_key", "key"},
      {"private_key_path", "/key.pem"},
      {"target_tickers", nlohmann::json::array()}};
  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);
  EXPECT_TRUE(config.target_tickers.empty());
}

TEST(ConfigTest, ThrowsOnNonexistentFile) {
  EXPECT_THROW(kalshi::load_config("/nonexistent/path/config.json"),
               std::runtime_error);
}
