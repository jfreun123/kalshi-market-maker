#pragma once

#include "order_manager.hpp"
#include "types.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kalshi {

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
// The manager auto-halts when realized PnL drops below daily_loss_limit.
class RiskManager {
public:
  explicit RiskManager(RiskLimits limits);

  // Returns false if halted or any limit would be breached by this order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  bool check_order(std::string_view ticker, Side side, int price_cents,
                   int quantity) const;

  // Snapshots open order counts, net positions, and realized PnL from om for
  // each ticker in the provided list. Auto-halts on daily loss breach.
  void update(const OrderManager &order_mgr,
              const std::vector<std::string> &tickers);

  [[nodiscard]] bool is_halted() const;
  void halt();
  void resume();

private:
  RiskLimits limits_;
  bool halted_{false};
  std::unordered_map<std::string, int> cached_position_;
  std::unordered_map<std::string, int> cached_open_order_count_;
  double cached_total_pnl_cents_{0.0};
};

} // namespace kalshi
