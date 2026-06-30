#include "config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace kalshi {

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
  config.target_tickers =
      json_data.value("target_tickers", std::vector<std::string>{});

  if (json_data.contains("quoter")) {
    const auto &quoter_json = json_data.at("quoter");
    config.quoter.target_spread_cents = quoter_json.value(
        "target_spread_cents", QuoterConfig::kDefaultTargetSpreadCents);
    config.quoter.skew_per_contract_cents = quoter_json.value(
        "skew_per_contract_cents", QuoterConfig::kDefaultSkewPerContractCents);
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
    config.quoter.view_debias_beta =
        quoter_json.value("view_debias_beta", ViewBasedModel::kDefaultBeta);
    config.quoter.maker_fee_rate =
        quoter_json.value("maker_fee_rate", QuoterConfig{}.maker_fee_rate);
  }

  if (json_data.contains("flow")) {
    const auto &flow_json = json_data.at("flow");
    config.flow.window_seconds = flow_json.value(
        "window_seconds", FlowImbalanceConfig::kDefaultWindowSeconds);
    config.flow.imbalance_ratio_threshold =
        flow_json.value("imbalance_ratio_threshold",
                        FlowImbalanceConfig::kDefaultRatioThreshold);
    config.flow.min_flow_volume = flow_json.value(
        "min_flow_volume", FlowImbalanceConfig::kDefaultMinFlowVolume);
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
  }

  return config;
}

} // namespace kalshi
