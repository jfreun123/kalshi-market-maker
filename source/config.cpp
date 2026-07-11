#include "config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace kalshi {

std::pair<int, int> quotable_price_band(const RiskLimits &risk,
                                        const QuoterConfig &quoter) {
  constexpr int kMinHalfSpread = 1;
  const int half_spread =
      std::max({kMinHalfSpread, quoter.target_spread_cents / 2,
                quoter.min_spread_cents / 2});
  return {risk.min_quote_price_cents + half_spread,
          risk.max_quote_price_cents - half_spread};
}

namespace {

template <typename ValueType>
ValueType require_field(const nlohmann::json &object,
                        const std::string &field_name) {
  if (!object.contains(field_name)) {
    throw std::runtime_error{"Config missing required field: " + field_name};
  }
  return object.at(field_name).template get<ValueType>();
}

constexpr auto kDefaultRestUrl = "https://trading-api.kalshi.com/trade-api/v2";
constexpr auto kDefaultWsUrl = "wss://trading-api.kalshi.com/trade-api/ws/v2";

} // namespace

AppConfig load_config(const std::filesystem::path &path) {
  std::ifstream file{path};
  if (!file) {
    throw std::runtime_error{"Cannot open config file: " + path.string()};
  }

  const nlohmann::json json_data = nlohmann::json::parse(file);

  AppConfig config;
  config.api_key = require_field<std::string>(json_data, "api_key");
  config.private_key_path =
      require_field<std::string>(json_data, "private_key_path");
  config.base_url = json_data.value("base_url", std::string{kDefaultRestUrl});
  config.ws_url = json_data.value("ws_url", std::string{kDefaultWsUrl});
  config.log_dir = json_data.value("log_dir", std::string{"logs"});
  config.pnl_state_path =
      json_data.value("pnl_state_path", std::string{"pnl_state.json"});
  config.account_wide_janitorial =
      json_data.value("account_wide_janitorial", true);
  config.target_tickers =
      json_data.value("target_tickers", std::vector<std::string>{});

  if (json_data.contains("quoter")) {
    const auto &quoter_json = json_data.at("quoter");
    config.quoter.target_spread_cents = quoter_json.value(
        "target_spread_cents", QuoterConfig::kDefaultTargetSpreadCents);
    config.quoter.reprice_threshold_cents = quoter_json.value(
        "reprice_threshold_cents", QuoterConfig::kDefaultRepriceThresholdCents);
    config.quoter.quote_size =
        quoter_json.value("quote_size", QuoterConfig::kDefaultQuoteSize);
    config.quoter.imbalance_spread_cents = quoter_json.value(
        "imbalance_spread_cents", QuoterConfig::kDefaultImbalanceSpreadCents);
    config.quoter.min_spread_cents = quoter_json.value(
        "min_spread_cents", QuoterConfig::kDefaultMinSpreadCents);
    config.quoter.use_view_based_pricing =
        quoter_json.value("use_view_based_pricing", false);
    config.quoter.use_clearing_pricing =
        quoter_json.value("use_clearing_pricing", false);
    config.quoter.clearing_tape_weight = quoter_json.value(
        "clearing_tape_weight", QuoterConfig::kDefaultClearingTapeWeight);
    config.quoter.tape_half_life_seconds = quoter_json.value(
        "tape_half_life_seconds", QuoterConfig::kDefaultTapeHalfLifeSeconds);
    config.quoter.view_debias_beta =
        quoter_json.value("view_debias_beta", ViewBasedModel::kDefaultBeta);
    config.quoter.maker_fee_rate =
        quoter_json.value("maker_fee_rate", QuoterConfig{}.maker_fee_rate);
    config.quoter.min_rest_ms =
        quoter_json.value("min_rest_ms", QuoterConfig::kDefaultMinRestMs);
    config.quoter.theo_jump_cents = quoter_json.value(
        "theo_jump_cents", QuoterConfig::kDefaultTheoJumpCents);
    config.quoter.fade_rest_ms =
        quoter_json.value("fade_rest_ms", QuoterConfig::kDefaultFadeRestMs);
    config.quoter.longshot_price_threshold_cents =
        quoter_json.value("longshot_price_threshold_cents",
                          QuoterConfig::kDefaultLongshotThresholdCents);
    config.quoter.longshot_edge_cents = quoter_json.value(
        "longshot_edge_cents", QuoterConfig::kDefaultLongshotEdgeCents);
    config.quoter.fv_ema_alpha =
        quoter_json.value("fv_ema_alpha", QuoterConfig::kDefaultFvEmaAlpha);
    config.quoter.winddown_seconds = quoter_json.value(
        "winddown_seconds", QuoterConfig::kDefaultWinddownSeconds);
    config.quoter.flow_lean_cents = quoter_json.value(
        "flow_lean_cents", QuoterConfig::kDefaultFlowLeanCents);
    config.quoter.inventory_cap_lots = quoter_json.value(
        "inventory_cap_lots", QuoterConfig::kDefaultInventoryCapLots);
    config.quoter.unwind_edge_cents = quoter_json.value(
        "unwind_edge_cents", QuoterConfig::kDefaultUnwindEdgeCents);
  }

  // The imbalance guard should engage once roughly one full quote of one-sided
  // flow is absorbed; an absolute floor above quote_size can never trigger
  // before the damage is done (demo finding D5).
  config.flow.min_flow_volume = config.quoter.quote_size;
  if (json_data.contains("flow")) {
    const auto &flow_json = json_data.at("flow");
    config.flow.window_seconds = flow_json.value(
        "window_seconds", FlowImbalanceConfig::kDefaultWindowSeconds);
    config.flow.imbalance_ratio_threshold =
        flow_json.value("imbalance_ratio_threshold",
                        FlowImbalanceConfig::kDefaultRatioThreshold);
    config.flow.min_flow_volume =
        flow_json.value("min_flow_volume", config.quoter.quote_size);
  }

  if (json_data.contains("risk")) {
    const auto &risk_json = json_data.at("risk");
    config.risk.max_position_per_market = risk_json.value(
        "max_position_per_market", RiskLimits::kDefaultMaxPosition);
    config.risk.max_open_orders_per_market = risk_json.value(
        "max_open_orders_per_market", RiskLimits::kDefaultMaxOpenOrders);
    config.risk.max_order_size =
        risk_json.value("max_order_size", RiskLimits::kDefaultMaxOrderSize);
    config.risk.daily_loss_limit =
        risk_json.value("daily_loss_limit", RiskLimits::kDefaultDailyLossLimit);
    config.risk.max_total_exposure_dollars = risk_json.value(
        "max_total_exposure_dollars", RiskLimits::kDefaultMaxTotalExposure);
    config.risk.max_total_loss_dollars = risk_json.value(
        "max_total_loss_dollars", RiskLimits::kDefaultMaxTotalLoss);
    config.risk.min_quote_price_cents = risk_json.value(
        "min_quote_price_cents", RiskLimits::kDefaultMinQuotePrice);
    config.risk.max_quote_price_cents = risk_json.value(
        "max_quote_price_cents", RiskLimits::kDefaultMaxQuotePrice);
    config.risk.max_drawdown_dollars = risk_json.value(
        "max_drawdown_dollars", RiskLimits::kDefaultMaxDrawdown);
  }

  if (json_data.contains("scanner")) {
    const auto &scanner_json = json_data.at("scanner");
    config.scanner.min_price_cents = scanner_json.value(
        "min_price_cents", ScannerConfig::kDefaultMinPriceCents);
    config.scanner.max_price_cents = scanner_json.value(
        "max_price_cents", ScannerConfig::kDefaultMaxPriceCents);
    config.scanner.min_spread_cents = scanner_json.value(
        "min_spread_cents", ScannerConfig::kDefaultMinSpreadCents);
    config.scanner.max_spread_cents = scanner_json.value(
        "max_spread_cents", ScannerConfig::kDefaultMaxSpreadCents);
    config.scanner.min_volume_24h = scanner_json.value(
        "min_volume_24h", ScannerConfig::kDefaultMinVolume24h);
    config.scanner.trade_top_n =
        scanner_json.value("trade_top_n", ScannerConfig::kDefaultTradeTopN);
    config.scanner.min_days_to_close = scanner_json.value(
        "min_days_to_close", ScannerConfig::kDefaultMinDaysToClose);
    config.scanner.max_days_to_close = scanner_json.value(
        "max_days_to_close", ScannerConfig::kDefaultMaxDaysToClose);
    config.scanner.max_stale_trade_minutes = scanner_json.value(
        "max_stale_trade_minutes", ScannerConfig::kDefaultMaxStaleTradeMinutes);
    config.scanner.rotation_minutes = scanner_json.value(
        "rotation_minutes", ScannerConfig::kDefaultRotationMinutes);
    config.scanner.min_trades_per_hour = scanner_json.value(
        "min_trades_per_hour", ScannerConfig::kDefaultMinTradesPerHour);
    config.scanner.min_trade_price_range_cents =
        scanner_json.value("min_trade_price_range_cents",
                           ScannerConfig::kDefaultMinTradePriceRangeCents);
    config.scanner.tape_range_lookback_minutes =
        scanner_json.value("tape_range_lookback_minutes",
                           ScannerConfig::kDefaultTapeRangeLookbackMinutes);
    config.scanner.min_minority_flow_ratio = scanner_json.value(
        "min_minority_flow_ratio", ScannerConfig::kDefaultMinMinorityFlowRatio);
    config.scanner.min_reversion_kappa = scanner_json.value(
        "min_reversion_kappa", ScannerConfig::kDefaultMinReversionKappa);
    config.scanner.reversion_window_minutes =
        scanner_json.value("reversion_window_minutes",
                           ScannerConfig::kDefaultReversionWindowMinutes);
  }

  const auto [quotable_min, quotable_max] =
      quotable_price_band(config.risk, config.quoter);
  config.scanner.min_price_cents =
      std::max(config.scanner.min_price_cents, quotable_min);
  config.scanner.max_price_cents =
      std::min(config.scanner.max_price_cents, quotable_max);

  return config;
}

} // namespace kalshi
