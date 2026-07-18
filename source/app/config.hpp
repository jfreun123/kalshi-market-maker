#pragma once

#include "engine/flow_imbalance.hpp"
#include "engine/risk_manager.hpp"
#include "exchange/ticker_scanner.hpp"
#include "strategy/quoter.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace kalshi {

struct AppConfig {
  std::string api_key;
  std::string private_key_path;
  std::string base_url;
  std::string ws_url;
  std::string log_dir{"logs"};
  std::string pnl_state_path{"pnl_state.json"};
  bool account_wide_janitorial{true};
  bool record_sessions{true};
  std::vector<std::string> target_tickers;
  QuoterConfig quoter;
  RiskLimits risk;
  ScannerConfig scanner;
  FlowImbalanceConfig flow;
};

std::pair<int, int> quotable_price_band(const RiskLimits &risk,
                                        const QuoterConfig &quoter);

// Loads configuration from a JSON file.
// Throws std::runtime_error if the file cannot be read or a required field
// is absent.
AppConfig load_config(const std::filesystem::path &path);

} // namespace kalshi
