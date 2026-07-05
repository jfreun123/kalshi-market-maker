#include "scan_output.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <ctime>
#include <fstream>

namespace kalshi {

namespace {

std::string to_iso8601(std::chrono::system_clock::time_point time_point) {
  const std::time_t time = std::chrono::system_clock::to_time_t(time_point);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::array<char, 32> buffer{}; // NOLINT(*-magic-numbers)
  const std::size_t written =
      std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string{buffer.data(), written};
}

} // namespace

std::string
scan_results_to_json(const std::vector<MarketScore> &results,
                     std::chrono::system_clock::time_point scanned_at) {
  nlohmann::json doc;
  doc["scanned_at"] = to_iso8601(scanned_at);
  doc["count"] = results.size();

  auto tickers = nlohmann::json::array();
  auto markets = nlohmann::json::array();
  for (const auto &result : results) {
    tickers.push_back(result.ticker);
    markets.push_back({{"ticker", result.ticker},
                       {"title", result.title},
                       {"category", result.category},
                       {"mid_price_cents", result.mid_price_cents},
                       {"spread_cents", result.spread_cents},
                       {"volume_24h", result.volume_24h},
                       {"days_to_close", result.days_to_close},
                       {"score", result.score}});
  }
  doc["tickers"] = std::move(tickers);
  doc["markets"] = std::move(markets);

  constexpr int kJsonIndent = 2;
  return doc.dump(kJsonIndent);
}

bool write_scan_results(const std::filesystem::path &path,
                        const std::vector<MarketScore> &results,
                        std::chrono::system_clock::time_point scanned_at) {
  std::ofstream file{path};
  if (!file) {
    return false;
  }
  file << scan_results_to_json(results, scanned_at);
  return file.good();
}

} // namespace kalshi
