#pragma once

#include "order_manager.hpp"
#include "types.hpp"

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
  kModelDiverge = 5,
  kManualHalt = 6,
  kConnectivity = 7,
};

struct RiskLimits {
  static constexpr int kDefaultMaxPosition = 100;
  static constexpr int kDefaultMaxOpenOrders = 4;
  static constexpr int kDefaultMaxOrderSize = 25;
  static constexpr double kDefaultDailyLossLimit = -500.0; // dollars

  int max_position_per_market = kDefaultMaxPosition;
  int max_open_orders_per_market = kDefaultMaxOpenOrders;
  int max_order_size = kDefaultMaxOrderSize;
  double daily_loss_limit = kDefaultDailyLossLimit; // dollars (negative = loss)
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
  void update(const OrderManager &order_mgr,
              const std::vector<std::string> &tickers);

  // Set / clear individual constraint bits. Any set bit causes is_halted().
  void set(Constraint bit);
  void clear(Constraint bit);
  [[nodiscard]] bool is_set(Constraint bit) const;

  // Returns a space-separated list of set constraint names; empty if none.
  [[nodiscard]] std::string active_constraints() const;

  [[nodiscard]] bool is_halted() const;
  void halt();   // sets kManualHalt
  void resume(); // clears all constraints

private:
  static constexpr std::size_t kNumConstraints = 8;

  RiskLimits limits_;
  std::bitset<kNumConstraints> constraints_;
  std::unordered_map<std::string, int> cached_position_;
  std::unordered_map<std::string, int> cached_open_order_count_;
  double cached_total_pnl_cents_{0.0};
};

} // namespace kalshi
