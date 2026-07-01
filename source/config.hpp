#pragma once

#include "flow_imbalance.hpp"
#include "quoter.hpp"
#include "risk_manager.hpp"
#include "ticker_scanner.hpp"

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
  std::vector<std::string> target_tickers;
  QuoterConfig quoter;
  RiskLimits risk;
  ScannerConfig scanner;
  FlowImbalanceConfig flow;
};

// The price band in which the quoter can rest a TWO-SIDED quote: the risk
// quote-price gate inset by the quoter's minimum half-spread, so both the bid
// (mid − half-spread) and ask (mid + half-spread) land inside the gate.
// load_config clamps the scanner's price band to this, so the scanner never
// surfaces markets the quoter can only quote one side of — or not at all.
// Returned as {min_cents, max_cents}.
std::pair<int, int> quotable_price_band(const RiskLimits &risk,
                                        const QuoterConfig &quoter);

// Loads configuration from a JSON file.
// Throws std::runtime_error if the file cannot be read or a required field
// is absent.
AppConfig load_config(const std::filesystem::path &path);

} // namespace kalshi
