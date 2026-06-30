#include "order_manager.hpp"

#include "ensure.hpp"
#include "logger.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kalshi {

constexpr int kContractMaxCents = 100;

OrderManager::OrderManager(RestClient &rest_client)
    : rest_client_{rest_client} {}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Order OrderManager::place(std::string_view ticker, Side side, int price_cents,
                          int quantity) {
  // Last line of defense before a value becomes a live order. A bad price or
  // size here means the quoter/risk logic upstream is broken, not a recoverable
  // input error — flatten and crash rather than send a malformed order.
  ensure(is_valid_price(price_cents), "order price outside [1,99]");
  ensure(quantity > 0, "order quantity must be positive");

  Order order = rest_client_.place_order(ticker, side, price_cents, quantity,
                                         OrderType::Limit);
  get_logger()->info("place ticker={} side={} price={} qty={} order_id={}",
                     ticker, (side == Side::Yes) ? "yes" : "no", price_cents,
                     quantity, order.id);
  open_orders_[order.id] = order;
  return order;
}

bool OrderManager::cancel(std::string_view order_id) {
  bool success = rest_client_.cancel_order(order_id);
  get_logger()->info("cancel order_id={} success={}", order_id, success);
  if (success) {
    open_orders_.erase(std::string{order_id});
  }
  return success;
}

void OrderManager::cancel_all(std::string_view ticker) {
  std::vector<std::string> to_cancel;
  for (const auto &[order_id, order] : open_orders_) {
    if (order.market_ticker == ticker) {
      to_cancel.push_back(order_id);
    }
  }
  // Best-effort: cancel as many as we can. A failure on one order (e.g. a
  // network error) must not prevent cancelling the rest, and must never throw —
  // callers rely on this to flatten quotes during shutdown and risk halts.
  for (const auto &order_id : to_cancel) {
    try {
      cancel(order_id);
    } catch (const std::exception &ex) {
      get_logger()->error("cancel_all: failed to cancel order_id={}: {}",
                          order_id, ex.what());
    }
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
  get_logger()->info("fill ticker={} order_id={} side={} price={} qty={}",
                     fill.market_ticker, fill.order_id,
                     (fill.side == Side::Yes) ? "yes" : "no", fill.price_cents,
                     fill.quantity);

  std::deque<Lot> &opposing_inventory = (fill.side == Side::Yes)
                                            ? no_lots_[fill.market_ticker]
                                            : yes_lots_[fill.market_ticker];
  std::deque<Lot> &same_inventory = (fill.side == Side::Yes)
                                        ? yes_lots_[fill.market_ticker]
                                        : no_lots_[fill.market_ticker];

  Quantity remaining = fill.quantity;
  while (remaining > kQuantityEpsilon && !opposing_inventory.empty()) {
    Lot &front = opposing_inventory.front();
    const Quantity matched = std::min(remaining, front.remaining);
    realized_pnl_[fill.market_ticker] +=
        static_cast<double>(kContractMaxCents - fill.price_cents -
                            front.price_cents) *
        matched;
    remaining -= matched;
    front.remaining -= matched;
    if (front.remaining <= kQuantityEpsilon) {
      opposing_inventory.pop_front();
    }
  }
  if (remaining > kQuantityEpsilon) {
    same_inventory.push_back({fill.price_cents, remaining});
  }

  // Update net position: YES = +qty, NO = -qty.
  const Quantity signed_qty =
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

Quantity OrderManager::net_position(std::string_view ticker) const {
  auto position_it = net_position_.find(std::string{ticker});
  return position_it == net_position_.end() ? 0 : position_it->second;
}

double OrderManager::realized_pnl(std::string_view ticker) const {
  auto pnl_it = realized_pnl_.find(std::string{ticker});
  return pnl_it == realized_pnl_.end() ? 0.0 : pnl_it->second;
}

double OrderManager::unrealized_pnl(std::string_view ticker,
                                    int yes_mid_cents) const {
  const std::string key{ticker};
  double unrealized = 0.0;

  // YES inventory marks at the YES mid: gain = (mid - cost) per contract.
  auto yes_it = yes_lots_.find(key);
  if (yes_it != yes_lots_.end()) {
    for (const Lot &lot : yes_it->second) {
      unrealized +=
          static_cast<double>(yes_mid_cents - lot.price_cents) * lot.remaining;
    }
  }

  // NO inventory marks at the complement (100 - YES mid).
  const int no_mark_cents = kContractMaxCents - yes_mid_cents;
  auto no_it = no_lots_.find(key);
  if (no_it != no_lots_.end()) {
    for (const Lot &lot : no_it->second) {
      unrealized +=
          static_cast<double>(no_mark_cents - lot.price_cents) * lot.remaining;
    }
  }

  return unrealized;
}

double OrderManager::position_cost(std::string_view ticker) const {
  const std::string key{ticker};
  double cost = 0.0;

  auto accumulate =
      [&cost](const std::unordered_map<std::string, std::deque<Lot>> &lots,
              const std::string &lookup) {
        auto iter = lots.find(lookup);
        if (iter == lots.end()) {
          return;
        }
        for (const Lot &lot : iter->second) {
          cost += static_cast<double>(lot.price_cents) * lot.remaining;
        }
      };
  accumulate(yes_lots_, key);
  accumulate(no_lots_, key);

  return cost;
}

ExposureDecomposition
OrderManager::exposure_decomposition(std::string_view ticker) const {
  const std::string key{ticker};

  // Sum the open inventory on one side: returns {contracts, total cost cents}.
  auto sum_side =
      [&key](const std::unordered_map<std::string, std::deque<Lot>> &lots)
      -> std::pair<Quantity, double> {
    auto iter = lots.find(key);
    if (iter == lots.end()) {
      return {0, 0.0};
    }
    Quantity quantity = 0;
    double cost = 0.0;
    for (const Lot &lot : iter->second) {
      quantity += lot.remaining;
      cost += static_cast<double>(lot.price_cents) * lot.remaining;
    }
    return {quantity, cost};
  };

  const auto [yes_qty, yes_cost] = sum_side(yes_lots_);
  const auto [no_qty, no_cost] = sum_side(no_lots_);

  ExposureDecomposition decomp;
  decomp.spread_capture_cents = realized_pnl(ticker);
  decomp.net_inventory = yes_qty - no_qty;

  // Inventory sits on at most one side; value the dominant side against its
  // winning outcome (each contract pays 100) and its losing outcome (pays 0).
  const Quantity held_qty = (yes_qty >= no_qty) ? yes_qty : no_qty;
  const double held_cost = (yes_qty >= no_qty) ? yes_cost : no_cost;
  decomp.e_win_cents = held_qty * kContractMaxCents - held_cost;
  decomp.e_loss_cents = -held_cost;
  return decomp;
}

const std::unordered_map<std::string, Order> &
OrderManager::open_orders() const {
  return open_orders_;
}

} // namespace kalshi
