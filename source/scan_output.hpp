#pragma once

#include "ticker_scanner.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace kalshi {

// Serializes ranked scan results to a JSON document (pretty-printed). The
// document carries scan metadata, a top-level "tickers" array in rank order
// (for easy extraction by downstream tooling), and a "markets" array with the
// full per-market detail.
[[nodiscard]] std::string
scan_results_to_json(const std::vector<MarketScore> &results,
                     std::chrono::system_clock::time_point scanned_at);

// Writes scan_results_to_json() to the given path. Returns false on I/O error.
[[nodiscard]] bool
write_scan_results(const std::filesystem::path &path,
                   const std::vector<MarketScore> &results,
                   std::chrono::system_clock::time_point scanned_at);


} // namespace kalshi
