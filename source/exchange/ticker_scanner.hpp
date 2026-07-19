#pragma once

#include "core/types.hpp"
#include "exchange/rest_client.hpp"
#include "exchange/scanner_config.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace kalshi {

struct MarketScore {
  std::string ticker;
  std::string title;
  std::string category;
  int mid_price_cents{0};
  int spread_cents{0};
  double volume_24h{0.0};
  double days_to_close{0.0};
  double incentive_reward_dollars{0.0};
  double score{0.0};
};

class TickerScanner {
public:
  explicit TickerScanner(RestClient &rest, ScannerConfig config = {});

  [[nodiscard]] std::vector<MarketScore>
  scan(int top_n = 20, std::chrono::system_clock::time_point now =
                           std::chrono::system_clock::now()) const;

private:
  void admit_finalists(std::vector<MarketScore> &candidates, int top_n,
                       std::chrono::system_clock::time_point now) const;
  [[nodiscard]] bool
  passes_flow_admission(const std::string &ticker,
                        std::chrono::system_clock::time_point now) const;
  [[nodiscard]] bool
  passes_reversion_admission(const std::string &ticker,
                             std::chrono::system_clock::time_point now) const;
  [[nodiscard]] bool
  tape_is_two_sided(const std::vector<PublicTrade> &trades,
                    std::chrono::system_clock::time_point hour_cutoff,
                    const std::string &ticker) const;

  [[nodiscard]] bool tape_shows_price_discovery(
      const std::vector<PublicTrade> &trades,
      std::chrono::system_clock::time_point lookback_cutoff,
      const std::string &ticker) const;
  [[nodiscard]] bool passes_book_admission(const std::string &ticker) const;

  RestClient &rest_;
  ScannerConfig config_;
  mutable std::vector<Market> cached_markets_;
  mutable std::chrono::system_clock::time_point cache_fetched_at_;
};

} // namespace kalshi
