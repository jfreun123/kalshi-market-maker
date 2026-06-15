#pragma once

#include "rest_client.hpp"
#include "types.hpp"

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kalshi {

// Tracks the lifecycle of all live orders and accumulates fill data.
//
// - place/cancel/cancel_all forward to RestClient and maintain local state.
// - record_fill updates net_position and realized_pnl using FIFO matching:
//   each incoming fill is matched against the oldest opposing-side lots.
//   Duplicate fills (same order_id + timestamp) are silently ignored.
class OrderManager {
public:
  explicit OrderManager(RestClient &rest_client);

  // Place a limit order and record it locally.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Order place(std::string_view ticker, Side side, int price_cents,
              int quantity);

  // Cancel an order. Returns true on success, false if the API rejected it.
  // Does not remove the order from open_orders on failure.
  bool cancel(std::string_view order_id);

  // Cancel all open orders for ticker. No-op for unknown tickers.
  void cancel_all(std::string_view ticker);

  // Process a fill event from the WebSocket feed.
  // Idempotent: duplicate fills (same order_id + timestamp) are ignored.
  void record_fill(const Fill &fill);

  // Net YES contracts for ticker (negative = net NO exposure).
  [[nodiscard]] int net_position(std::string_view ticker) const;

  // Cumulative PnL in cents for matched (YES + NO) position pairs.
  // Computed via FIFO: each NO fill that closes a YES lot (or vice versa)
  // contributes (100 - yes_price - no_price) * matched_qty cents.
  [[nodiscard]] double realized_pnl(std::string_view ticker) const;

  [[nodiscard]] const std::unordered_map<std::string, Order> &
  open_orders() const;

private:
  struct Lot {
    int price_cents;
    int remaining;
  };

  static std::string fill_key(const Fill &fill);

  RestClient &rest_client_;
  std::unordered_map<std::string, Order> open_orders_;
  std::unordered_map<std::string, int> net_position_;
  std::unordered_map<std::string, double> realized_pnl_;
  std::unordered_map<std::string, std::deque<Lot>> yes_lots_;
  std::unordered_map<std::string, std::deque<Lot>> no_lots_;
  std::unordered_set<std::string> seen_fills_;
};

} // namespace kalshi
