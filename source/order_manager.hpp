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

// Interface for order lifecycle operations. Quoter and RiskManager depend on
// this abstraction rather than the concrete RestClient-backed OrderManager,
// which allows unit testing without HTTP and enables alternative
// implementations (paper trading, multi-exchange routing, rate-limited
// wrappers).
class IOrderManager {
public:
  virtual ~IOrderManager() = default;

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  virtual Order place(std::string_view ticker, Side side, int price_cents,
                      int quantity) = 0;
  virtual bool cancel(std::string_view order_id) = 0;
  virtual void cancel_all(std::string_view ticker) = 0;
  virtual void record_fill(const Fill &fill) = 0;

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

// Tracks the lifecycle of all live orders and accumulates fill data.
//
// - place/cancel/cancel_all forward to RestClient and maintain local state.
// - record_fill updates net_position and realized_pnl using FIFO matching:
//   each incoming fill is matched against the oldest opposing-side lots.
//   Duplicate fills (same order_id + timestamp) are silently ignored.
class OrderManager : public IOrderManager {
public:
  explicit OrderManager(RestClient &rest_client);

  ~OrderManager() override = default;
  OrderManager(const OrderManager &) = delete;
  OrderManager &operator=(const OrderManager &) = delete;
  OrderManager(OrderManager &&) = delete;
  OrderManager &operator=(OrderManager &&) = delete;

  Order place(std::string_view ticker, Side side, int price_cents,
              int quantity) override;
  bool cancel(std::string_view order_id) override;
  void cancel_all(std::string_view ticker) override;
  void record_fill(const Fill &fill) override;

  [[nodiscard]] Quantity net_position(std::string_view ticker) const override;
  [[nodiscard]] double realized_pnl(std::string_view ticker) const override;
  [[nodiscard]] double fees_paid(std::string_view ticker) const override;
  [[nodiscard]] double unrealized_pnl(std::string_view ticker,
                                      int yes_mid_cents) const override;
  [[nodiscard]] double position_cost(std::string_view ticker) const override;
  [[nodiscard]] ExposureDecomposition
  exposure_decomposition(std::string_view ticker) const override;
  [[nodiscard]] const std::unordered_map<std::string, Order> &
  open_orders() const override;

private:
  struct Lot {
    int price_cents;
    Quantity remaining;
  };

  static std::string fill_key(const Fill &fill);

  [[nodiscard]] double realized_spread(std::string_view ticker) const;

  RestClient &rest_client_;
  std::unordered_map<std::string, Order> open_orders_;
  std::unordered_map<std::string, Quantity> net_position_;
  std::unordered_map<std::string, double> realized_spread_;
  std::unordered_map<std::string, double> fees_paid_;
  std::unordered_map<std::string, std::deque<Lot>> yes_lots_;
  std::unordered_map<std::string, std::deque<Lot>> no_lots_;
  std::unordered_set<std::string> seen_fills_;
};

} // namespace kalshi
