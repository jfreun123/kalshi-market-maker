#pragma once

#include "flow_imbalance.hpp"
#include "quoter.hpp"
#include "risk_manager.hpp"
#include "ticker_scanner.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace kalshi {

struct AppConfig {
  std::string api_key;
  std::string private_key_path;
  std::string base_url;
  std::string ws_url;
  std::string log_dir{"logs"};
  std::vector<std::string> target_tickers;
  QuoterConfig quoter;
  RiskLimits risk;
  ScannerConfig scanner;
  FlowImbalanceConfig flow;
};

// Loads configuration from a JSON file.
// Throws std::runtime_error if the file cannot be read or a required field
// is absent.
AppConfig load_config(const std::filesystem::path &path);

} // namespace kalshi
