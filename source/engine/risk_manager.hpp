#pragma once

#include "core/types.hpp"
#include "engine/order_manager_iface.hpp"
#include "engine/portfolio.hpp"
#include "engine/risk_limits.hpp"

#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kalshi {

// Named risk constraints. Each bit in RiskManager's bitset corresponds to one.
// Bits with different clearing semantics:
//   kPnLLimit / kManualHalt — require explicit resume() or clear()
//   kHighFillRate            — auto-cleared after cooldown by main loop
//   kStaleBook               — auto-cleared on next WS message
//   kPositionLimit / kOpenOrders / kModelDiverge / kConnectivity — as needed
enum class Constraint : uint8_t {
  kPnLLimit = 0,
  kPositionLimit = 1,
  kOpenOrders = 2,
  kHighFillRate = 3,
  kStaleBook = 4,
  kModelDiverge = 5, // local accounting diverged from exchange (reconciliation)
  kManualHalt = 6,
  kConnectivity = 7,
  kOverExposure = 8,  // total capital at risk exceeded the portfolio cap
  kPortfolioLoss = 9, // total PnL (realized + unrealized) breached the loss cap
  kDrawdown = 10,     // total PnL fell too far from its high-water mark
};

// Pre-trade risk checks for all outgoing orders.
//
// Call update(om, tickers) each cycle to snapshot current positions and PnL.
// Call check_order() before every place() call.
// The manager auto-halts (sets kPnLLimit) when realized PnL drops below
// daily_loss_limit. Any constraint bit being set causes is_halted() == true.
class RiskManager {
public:
  explicit RiskManager(RiskLimits limits);

  // Returns false if halted or any limit would be breached by this order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  bool check_order(std::string_view ticker, Side side, int price_cents,
                   int quantity) const;

  // Snapshots open order counts, net positions, and realized PnL from om for
  // each ticker in the provided list. Sets kPnLLimit on daily loss breach.
  void update(const IOrderManager &order_mgr,
              const std::vector<std::string> &tickers);

  // Portfolio-level kill-switch. Consumes the Portfolio read-model (the single
  // aggregation authority) rather than re-summing positions. Sets kOverExposure
  // when total capital at risk exceeds max_total_exposure_dollars, and
  // kPortfolioLoss when total PnL (realized + unrealized) falls below
  // max_total_loss_dollars. Only sets bits; clearing requires resume().
  void update_portfolio(const PortfolioSnapshot &snapshot);

  // Set / clear individual constraint bits. Any set bit causes is_halted().
  void set(Constraint bit);
  void clear(Constraint bit);
  [[nodiscard]] bool is_set(Constraint bit) const;

  // Returns a space-separated list of set constraint names; empty if none.
  [[nodiscard]] std::string active_constraints() const;

  [[nodiscard]] bool is_halted() const;
  void halt();   // sets kManualHalt
  void resume(); // clears all constraints

  [[nodiscard]] const RiskLimits &limits() const { return limits_; }

private:
  static constexpr std::size_t kNumConstraints = 11;

  RiskLimits limits_;
  std::bitset<kNumConstraints> constraints_;
  std::unordered_map<std::string, Quantity> cached_position_;
  std::unordered_map<std::string, int> cached_open_order_count_;
  double cached_total_pnl_cents_{0.0};
  // High-water mark of total PnL (cents) seen by update_portfolio. Starts at
  // session break-even (0); resume() re-anchors it to the next observation.
  double peak_total_pnl_cents_{0.0};
};

} // namespace kalshi
