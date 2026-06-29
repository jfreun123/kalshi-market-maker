#pragma once

#include "order_manager.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kalshi {

// Maps a market ticker to its current YES mid price in cents. Built from the
// live orderbook map; used to mark open inventory to market.
using MarkMap = std::unordered_map<std::string, int>;

// Aggregated exposure for one event (a group of correlated strike markets).
struct EventExposure {
  std::string event_ticker;
  double realized_pnl_cents{0.0};
  double unrealized_pnl_cents{0.0};
  double notional_cost_cents{0.0}; // capital at risk across the event's markets
  int market_count{0};

  [[nodiscard]] double total_pnl_cents() const {
    return realized_pnl_cents + unrealized_pnl_cents;
  }
};

// A point-in-time view of the whole book: totals plus a per-event breakdown.
struct PortfolioSnapshot {
  double total_realized_cents{0.0};
  double total_unrealized_cents{0.0};
  double total_notional_cents{0.0};    // total capital at risk
  std::vector<EventExposure> by_event; // sorted by notional_cost descending

  [[nodiscard]] double total_pnl_cents() const {
    return total_realized_cents + total_unrealized_cents;
  }
};

// Derives the event ticker from a market ticker by stripping the final
// "-<strike>" segment: "KXFED-26SEP-T3.00" -> "KXFED-26SEP". A ticker with no
// hyphen is returned unchanged.
[[nodiscard]] std::string event_ticker_of(std::string_view market_ticker);

// Pure read-model. Aggregates positions, realized and unrealized PnL, and
// capital at risk from an IOrderManager across a supplied set of tickers,
// marking open inventory with the supplied marks. Owns no state and places no
// orders — it is a view over the OrderManager (the single source of truth).
class Portfolio {
public:
  explicit Portfolio(const IOrderManager &order_mgr);

  [[nodiscard]] PortfolioSnapshot
  snapshot(const std::vector<std::string> &tickers, const MarkMap &marks) const;

private:
  const IOrderManager &order_mgr_;
};

} // namespace kalshi
