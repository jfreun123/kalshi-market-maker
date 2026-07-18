#pragma once

// Rolling per-market window of public trade prints — the live tape. Feeds the
// tape half of clearing-price fair value (BETTER_PRICING.md §3b) and the
// two-sided-flow admission ratio (item 65). Own fills are identified by
// trade_id in either arrival order and weighted into the VWAP by
// own_fill_weight (default 0 = excluded, so fv never confirms our own
// quotes; the taker who hit us is real flow, so the backtest may prefer a
// partial weight); print_count and minority_side_ratio always exclude them.
// Queries take `now` so they are testable and side-effect-free; record_trade
// evicts prints older than the window to bound memory.

#include "core/types.hpp"

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
  static constexpr double kDefaultOwnFillWeight = 0.0;

  int window_seconds = kDefaultWindowSeconds;
  double own_fill_weight = kDefaultOwnFillWeight;
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
  [[nodiscard]] bool is_in_window(const PublicTrade &print,
                                  TimePoint now) const;
  [[nodiscard]] bool is_countable(const PublicTrade &print,
                                  TimePoint now) const;

  TradeTapeConfig config_;
  std::unordered_map<std::string, std::deque<PublicTrade>> prints_by_ticker_;
  std::unordered_set<std::string> own_trade_ids_;
};

} // namespace kalshi
