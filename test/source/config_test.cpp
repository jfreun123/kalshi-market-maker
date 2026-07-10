#include "config.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path write_temp_config(const nlohmann::json &content) {
  static std::atomic<int> file_counter{0};
  const auto file_name = "kalshi_config_test_" + std::to_string(getpid()) +
                         "_" + std::to_string(file_counter++) + ".json";
  const auto path = std::filesystem::temp_directory_path() / file_name;
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
  constexpr int kMinRestMs = 5'000;
  constexpr int kTheoJumpCents = 4;
  constexpr int kFadeRestMs = 750;
  constexpr int kLongshotThreshold = 35;
  constexpr int kLongshotEdge = 2;
  constexpr double kFvEmaAlpha = 0.35;
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
        {"reprice_threshold_cents", kRepriceCents},
        {"quote_size", kQuoteSize},
        {"imbalance_spread_cents", kImbalanceSpread},
        {"min_spread_cents", kMinSpread},
        {"use_view_based_pricing", kUseViewBased},
        {"view_debias_beta", kViewBeta},
        {"maker_fee_rate", kMakerFee},
        {"min_rest_ms", kMinRestMs},
        {"theo_jump_cents", kTheoJumpCents},
        {"fade_rest_ms", kFadeRestMs},
        {"longshot_price_threshold_cents", kLongshotThreshold},
        {"longshot_edge_cents", kLongshotEdge},
        {"fv_ema_alpha", kFvEmaAlpha},
        {"winddown_seconds", 30},
        {"unwind_edge_cents", 1.5}}},
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
  EXPECT_EQ(config.quoter.reprice_threshold_cents, kRepriceCents);
  EXPECT_EQ(config.quoter.quote_size, kQuoteSize);
  EXPECT_EQ(config.quoter.imbalance_spread_cents, kImbalanceSpread);
  EXPECT_EQ(config.quoter.min_spread_cents, kMinSpread);
  EXPECT_EQ(config.quoter.use_view_based_pricing, kUseViewBased);
  EXPECT_DOUBLE_EQ(config.quoter.view_debias_beta, kViewBeta);
  EXPECT_DOUBLE_EQ(config.quoter.maker_fee_rate, kMakerFee);
  EXPECT_EQ(config.quoter.min_rest_ms, kMinRestMs);
  EXPECT_EQ(config.quoter.theo_jump_cents, kTheoJumpCents);
  EXPECT_EQ(config.quoter.fade_rest_ms, kFadeRestMs);
  EXPECT_EQ(config.quoter.longshot_price_threshold_cents, kLongshotThreshold);
  EXPECT_EQ(config.quoter.longshot_edge_cents, kLongshotEdge);
  EXPECT_DOUBLE_EQ(config.quoter.fv_ema_alpha, kFvEmaAlpha);
  EXPECT_EQ(config.quoter.winddown_seconds, 30);
  EXPECT_DOUBLE_EQ(config.quoter.unwind_edge_cents, 1.5);
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
  constexpr int kMinPrice = 15;
  constexpr int kMaxPrice = 85;
  constexpr int kMinSpread = 1;
  constexpr int kMaxSpread = 20;
  constexpr double kMinVolume = 25.0;
  constexpr double kMinDays = 0.5;
  constexpr double kMaxDays = 120.0;

  nlohmann::json config_json = minimal_config_json();
  config_json["scanner"] = {{"min_price_cents", kMinPrice},
                            {"max_price_cents", kMaxPrice},
                            {"min_spread_cents", kMinSpread},
                            {"max_spread_cents", kMaxSpread},
                            {"min_volume_24h", kMinVolume},
                            {"min_days_to_close", kMinDays},
                            {"max_days_to_close", kMaxDays},
                            {"max_stale_trade_minutes", 45},
                            {"rotation_minutes", 7},
                            {"min_trades_per_hour", 9},
                            {"min_trade_price_range_cents", 4},
                            {"tape_range_lookback_minutes", 240}};

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
  EXPECT_EQ(config.scanner.max_stale_trade_minutes, 45);
  EXPECT_EQ(config.scanner.rotation_minutes, 7);
  EXPECT_EQ(config.scanner.min_trades_per_hour, 9);
  EXPECT_EQ(config.scanner.min_trade_price_range_cents, 4);
  EXPECT_EQ(config.scanner.tape_range_lookback_minutes, 240);
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
  EXPECT_EQ(config.flow.min_flow_volume, config.quoter.quote_size);
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

TEST(ConfigTest, MinFlowVolumeDefaultsToConfiguredQuoteSize) {
  constexpr int kSmallQuoteSize = 7;
  auto config_json = minimal_config_json();
  config_json["quoter"] = {{"quote_size", kSmallQuoteSize}};
  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.flow.min_flow_volume, kSmallQuoteSize)
      << "the guard must engage once ~one quote of one-sided flow is absorbed";
}

TEST(ConfigTest, MinFlowVolumeDefaultsToQuoteSizeWhenFlowSectionOmitsIt) {
  constexpr int kSmallQuoteSize = 7;
  auto config_json = minimal_config_json();
  config_json["quoter"] = {{"quote_size", kSmallQuoteSize}};
  config_json["flow"] = {{"window_seconds", 60}};
  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.flow.min_flow_volume, kSmallQuoteSize);
}

TEST(ConfigTest, ExplicitMinFlowVolumeOverridesQuoteSizeDefault) {
  constexpr int kSmallQuoteSize = 7;
  constexpr int kExplicitFloor = 25;
  auto config_json = minimal_config_json();
  config_json["quoter"] = {{"quote_size", kSmallQuoteSize}};
  config_json["flow"] = {{"min_flow_volume", kExplicitFloor}};
  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.flow.min_flow_volume, kExplicitFloor);
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

TEST(ConfigTest, QuotablePriceBandInsetsRiskGateByMinHalfSpread) {
  kalshi::RiskLimits risk;
  risk.min_quote_price_cents = 10;
  risk.max_quote_price_cents = 90;
  kalshi::QuoterConfig quoter;

  const auto [band_min, band_max] = kalshi::quotable_price_band(risk, quoter);

  EXPECT_EQ(band_min, 12);
  EXPECT_EQ(band_max, 88);
}

TEST(ConfigTest, ScannerBandClampedToQuotableRange) {
  nlohmann::json config_json = minimal_config_json();
  config_json["scanner"] = {{"min_price_cents", 2}, {"max_price_cents", 98}};

  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.scanner.min_price_cents, 12);
  EXPECT_EQ(config.scanner.max_price_cents, 88);
}

TEST(ConfigTest, ScannerBandNotWidenedWhenTighterThanQuotable) {
  nlohmann::json config_json = minimal_config_json();
  config_json["scanner"] = {{"min_price_cents", 30}, {"max_price_cents", 70}};

  const auto path = write_temp_config(config_json);
  const auto config = kalshi::load_config(path);
  std::filesystem::remove(path);

  EXPECT_EQ(config.scanner.min_price_cents, 30);
  EXPECT_EQ(config.scanner.max_price_cents, 70);
}

TEST(ConfigTest, ThrowsOnNonexistentFile) {
  EXPECT_THROW(kalshi::load_config("/nonexistent/path/config.json"),
               std::runtime_error);
}
