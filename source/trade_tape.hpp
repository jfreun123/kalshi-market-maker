#pragma once

// Rolling per-market window of public trade prints — the live tape. Feeds the
// tape half of clearing-price fair value (BETTER_PRICING.md §3b) and the
// two-sided-flow admission ratio (item 65). Own fills are excluded by
// trade_id in either arrival order, so fv never confirms our own quotes.
// Queries take `now` so they are testable and side-effect-free; record_trade
// evicts prints older than the window to bound memory.

#include "types.hpp"

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace kalshi {

struct TradeTapeConfig {
  static constexpr int kDefaultWindowSeconds = 300;

  int window_seconds = kDefaultWindowSeconds;
};

class TradeTape {
public:
  using Clock = std::chrono::system_clock;
  using TimePoint = Clock::time_point;

  explicit TradeTape(TradeTapeConfig config);

  void record_trade(const PublicTrade &trade);
  void record_own_fill(const std::string &trade_id);

  [[nodiscard]] std::optional<double> vwap_cents(std::string_view ticker,
                                                 std::chrono::seconds half_life,
                                                 TimePoint now) const;
  [[nodiscard]] int print_count(std::string_view ticker, TimePoint now) const;
  [[nodiscard]] std::optional<double>
  minority_side_ratio(std::string_view ticker, TimePoint now) const;

private:
  [[nodiscard]] bool is_countable(const PublicTrade &print,
                                  TimePoint now) const;

  TradeTapeConfig config_;
  std::unordered_map<std::string, std::deque<PublicTrade>> prints_by_ticker_;
  std::unordered_set<std::string> own_trade_ids_;
};

} // namespace kalshi
