#include "order_manager.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace kalshi {

OrderManager::OrderManager(RestClient &rest_client)
    : rest_client_{rest_client} {}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Order OrderManager::place(const std::string &ticker, Side side, int price_cents,
                          int quantity) {
  Order order = rest_client_.place_order(ticker, side, price_cents, quantity,
                                         OrderType::Limit);
  open_orders_[order.id] = order;
  return order;
}

bool OrderManager::cancel(const std::string &order_id) {
  bool success = rest_client_.cancel_order(order_id);
  if (success) {
    open_orders_.erase(order_id);
  }
  return success;
}

void OrderManager::cancel_all(const std::string &ticker) {
  std::vector<std::string> to_cancel;
  for (const auto &[order_id, order] : open_orders_) {
    if (order.market_ticker == ticker) {
      to_cancel.push_back(order_id);
    }
  }
  for (const auto &order_id : to_cancel) {
    cancel(order_id);
  }
}

std::string OrderManager::fill_key(const Fill &fill) {
  return fill.order_id + "@" +
         std::to_string(fill.timestamp.time_since_epoch().count());
}

void OrderManager::record_fill(const Fill &fill) {
  if (!seen_fills_.insert(fill_key(fill)).second) {
    return; // Duplicate fill; ignore.
  }

  std::deque<Lot> &opposing_inventory = (fill.side == Side::Yes)
                                            ? no_lots_[fill.market_ticker]
                                            : yes_lots_[fill.market_ticker];
  std::deque<Lot> &same_inventory = (fill.side == Side::Yes)
                                        ? yes_lots_[fill.market_ticker]
                                        : no_lots_[fill.market_ticker];

  int remaining = fill.quantity;
  while (remaining > 0 && !opposing_inventory.empty()) {
    Lot &front = opposing_inventory.front();
    const int matched = std::min(remaining, front.remaining);
    realized_pnl_[fill.market_ticker] +=
        static_cast<double>(100 - fill.price_cents - front.price_cents) *
        matched;
    remaining -= matched;
    front.remaining -= matched;
    if (front.remaining == 0) {
      opposing_inventory.pop_front();
    }
  }
  if (remaining > 0) {
    same_inventory.push_back({fill.price_cents, remaining});
  }

  // Update net position: YES = +qty, NO = -qty.
  const int signed_qty =
      (fill.side == Side::Yes) ? fill.quantity : -fill.quantity;
  net_position_[fill.market_ticker] += signed_qty;

  // Update the order's filled quantity if it's still open.
  auto order_iter = open_orders_.find(fill.order_id);
  if (order_iter != open_orders_.end()) {
    order_iter->second.filled_quantity += fill.quantity;
    if (order_iter->second.filled_quantity >= order_iter->second.quantity) {
      order_iter->second.status = OrderStatus::Filled;
      open_orders_.erase(order_iter);
    } else {
      order_iter->second.status = OrderStatus::PartiallyFilled;
    }
  }
}

int OrderManager::net_position(const std::string &ticker) const {
  auto position_it = net_position_.find(ticker);
  return position_it == net_position_.end() ? 0 : position_it->second;
}

double OrderManager::realized_pnl(const std::string &ticker) const {
  auto pnl_it = realized_pnl_.find(ticker);
  return pnl_it == realized_pnl_.end() ? 0.0 : pnl_it->second;
}

const std::unordered_map<std::string, Order> &
OrderManager::open_orders() const {
  return open_orders_;
}

} // namespace kalshi
