#pragma once

#include "rest_client.hpp"
#include "types.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace kalshi {

struct ScannerConfig {
  int min_price_cents{15};
  int max_price_cents{85};
  int min_spread_cents{3};
  int max_spread_cents{10};
  double min_volume_usd{1000.0};
  double min_days_to_close{1.0};
  double max_days_to_close{90.0};
};

struct MarketScore {
  std::string ticker;
  std::string title;
  std::string category;
  int mid_price_cents{0};
  int spread_cents{0};
  double volume_usd{0.0};
  double days_to_close{0.0};
  double score{0.0};
};

class TickerScanner {
public:
  explicit TickerScanner(RestClient &rest, ScannerConfig config = {});

  [[nodiscard]] std::vector<MarketScore>
  scan(int top_n = 20, std::chrono::system_clock::time_point now =
                           std::chrono::system_clock::now()) const;

private:
  RestClient &rest_;
  ScannerConfig config_;
};

} // namespace kalshi
