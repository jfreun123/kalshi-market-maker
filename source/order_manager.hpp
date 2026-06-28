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

  [[nodiscard]] virtual int net_position(std::string_view ticker) const = 0;
  [[nodiscard]] virtual double realized_pnl(std::string_view ticker) const = 0;
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

  [[nodiscard]] int net_position(std::string_view ticker) const override;
  [[nodiscard]] double realized_pnl(std::string_view ticker) const override;
  [[nodiscard]] const std::unordered_map<std::string, Order> &
  open_orders() const override;

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
