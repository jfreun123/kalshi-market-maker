#pragma once

#include "order_manager_iface.hpp"
#include "rest_client.hpp"
#include "types.hpp"

#include <deque>
#include <optional>
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
//   Duplicate fills (same trade_id, or order_id + timestamp when the trade_id
//   is missing) are ignored; the return value says whether the fill was new.
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
  std::optional<std::string> amend(std::string_view order_id,
                                   std::string_view ticker, Side side,
                                   int new_price_cents,
                                   Quantity count) override;
  void cancel_all(std::string_view ticker) override;
  bool record_fill(const Fill &fill) override;

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
