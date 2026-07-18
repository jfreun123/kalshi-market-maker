#pragma once
// Order-lifecycle interface consumed by the strategy/risk/session layers.
// Depending on this abstraction rather than the concrete RestClient-backed
// OrderManager keeps those layers free of the exchange client stack, allows
// unit testing without HTTP, and enables alternative implementations (paper
// trading, multi-exchange routing, rate-limited wrappers).

#include "types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kalshi {

// Decomposes a ticker's position into the spread the bot has locked in (from
// matched YES/NO pairs — outcome-independent profit) and its remaining
// directional exposure. Per Palumbo, that directional bet (E_win) — not spread
// capture — dominates LP terminal P&L, so it is worth tracking on its own. All
// figures in cents. Open inventory sits on at most one side at a time
// (offsetting fills realize against the opposing inventory first).
struct ExposureDecomposition {
  double spread_capture_cents{
      0.0};                 // realized profit from matched complete sets
  Quantity net_inventory{}; // signed open contracts (+YES / -NO)
  double e_win_cents{0.0};  // payoff if the held side WINS
  double e_loss_cents{0.0}; // payoff if it LOSES (≤ 0; -capital at risk)
};

class IOrderManager {
public:
  virtual ~IOrderManager() = default;

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  virtual Order place(std::string_view ticker, Side side, int price_cents,
                      int quantity) = 0;
  virtual bool cancel(std::string_view order_id) = 0;
  virtual std::optional<std::string> amend(std::string_view order_id,
                                           std::string_view ticker, Side side,
                                           int new_price_cents,
                                           Quantity count) = 0;
  virtual void cancel_all(std::string_view ticker) = 0;
  virtual bool record_fill(const Fill &fill) = 0;

  [[nodiscard]] virtual Quantity
  net_position(std::string_view ticker) const = 0;
  [[nodiscard]] virtual double realized_pnl(std::string_view ticker) const = 0;
  [[nodiscard]] virtual double fees_paid(std::string_view ticker) const = 0;

  // Mark-to-market PnL of open inventory at the given YES mid price (cents).
  // YES lots mark at yes_mid; NO lots mark at (100 - yes_mid).
  [[nodiscard]] virtual double unrealized_pnl(std::string_view ticker,
                                              int yes_mid_cents) const = 0;

  // Capital currently at risk in open inventory: sum of (remaining * cost) for
  // all open lots, in cents. This is the most a long binary position can lose.
  [[nodiscard]] virtual double position_cost(std::string_view ticker) const = 0;

  // Splits the position into locked spread capture and directional
  // E_win/E_loss.
  [[nodiscard]] virtual ExposureDecomposition
  exposure_decomposition(std::string_view ticker) const = 0;

  [[nodiscard]] virtual const std::unordered_map<std::string, Order> &
  open_orders() const = 0;
};

} // namespace kalshi
