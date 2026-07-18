#include "engine/orderbook.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kalshi {

namespace {

auto find_level(std::vector<Level> &levels,
                int price_cents) -> std::vector<Level>::iterator {
  auto level_pos = std::lower_bound(
      levels.begin(), levels.end(), Level{price_cents, Quantity{}},
      [](const Level &level, const Level &target) {
        return level.price_cents > target.price_cents;
      });
  if (level_pos != levels.end() && level_pos->price_cents == price_cents) {
    return level_pos;
  }
  return levels.end();
}

auto insertion_point(std::vector<Level> &levels,
                     int price_cents) -> std::vector<Level>::iterator {
  return std::lower_bound(levels.begin(), levels.end(),
                          Level{price_cents, Quantity{}},
                          [](const Level &level, const Level &target) {
                            return level.price_cents > target.price_cents;
                          });
}

} // namespace

void LocalOrderbook::apply_snapshot(const Orderbook &snap) {
  state_ = snap;
  const auto by_price_descending = [](const Level &left, const Level &right) {
    return left.price_cents > right.price_cents;
  };
  std::sort(state_.yes.begin(), state_.yes.end(), by_price_descending);
  std::sort(state_.no.begin(), state_.no.end(), by_price_descending);
}

void LocalOrderbook::apply_delta(Side side, int price_cents, Quantity delta) {
  auto &levels = (side == Side::Yes) ? state_.yes : state_.no;
  auto existing = find_level(levels, price_cents);

  if (existing != levels.end()) {
    existing->quantity += delta;
    if (existing->quantity <= Quantity{}) {
      levels.erase(existing);
    }
    return;
  }

  if (delta.is_positive()) {
    auto insert_pos = insertion_point(levels, price_cents);
    levels.insert(insert_pos, {price_cents, delta});
  }
}

std::optional<Level> LocalOrderbook::best_bid() const {
  if (state_.yes.empty()) {
    return std::nullopt;
  }
  return state_.yes.front();
}

std::optional<Level> LocalOrderbook::best_ask() const {
  if (state_.no.empty()) {
    return std::nullopt;
  }
  const Level &best_no = state_.no.front();
  return Level{complement_price(best_no.price_cents), best_no.quantity};
}

double LocalOrderbook::mid_price_cents() const {
  auto bid = best_bid();
  auto ask = best_ask();
  if (!bid || !ask) {
    return 0.0;
  }
  constexpr double kTwoSides = 2.0;
  return (static_cast<double>(bid->price_cents) +
          static_cast<double>(ask->price_cents)) /
         kTwoSides;
}

double LocalOrderbook::micro_price_cents() const {
  auto bid = best_bid();
  auto ask = best_ask();
  if (!bid || !ask) {
    return 0.0;
  }
  const double bid_qty = bid->quantity.contracts();
  const double ask_qty = ask->quantity.contracts();
  const double total_qty = bid_qty + ask_qty;
  if (total_qty <= 0.0) {
    return mid_price_cents();
  }
  return ((static_cast<double>(bid->price_cents) * ask_qty) +
          (static_cast<double>(ask->price_cents) * bid_qty)) /
         total_qty;
}

double
LocalOrderbook::clearing_price_cents(const DepthWeighting &weighting) const {
  if (state_.yes.empty() || state_.no.empty()) {
    return micro_price_cents();
  }
  const double mid = mid_price_cents();
  struct SideDepth {
    double weight_total{0.0};
    double weighted_price_sum{0.0};
  };
  const auto accumulate_side = [&](const std::vector<Level> &levels,
                                   bool bids) {
    SideDepth depth;
    int levels_used = 0;
    for (const auto &level : levels) {
      if (weighting.max_levels > 0 && levels_used >= weighting.max_levels) {
        break;
      }
      const double price =
          bids ? level.price_cents : complement_price(level.price_cents);
      const double distance_cents = bids ? (mid - price) : (price - mid);
      const double weight = level.quantity.contracts() *
                            std::pow(weighting.decay_per_cent, distance_cents);
      depth.weight_total += weight;
      depth.weighted_price_sum += weight * price;
      ++levels_used;
    }
    return depth;
  };
  const SideDepth bid_depth = accumulate_side(state_.yes, true);
  const SideDepth ask_depth = accumulate_side(state_.no, false);
  if (bid_depth.weight_total <= 0.0 || ask_depth.weight_total <= 0.0) {
    return micro_price_cents();
  }
  const double bid_avg_price =
      bid_depth.weighted_price_sum / bid_depth.weight_total;
  const double ask_avg_price =
      ask_depth.weighted_price_sum / ask_depth.weight_total;
  return ((bid_avg_price * ask_depth.weight_total) +
          (ask_avg_price * bid_depth.weight_total)) /
         (bid_depth.weight_total + ask_depth.weight_total);
}

int LocalOrderbook::spread_cents() const {
  auto bid = best_bid();
  auto ask = best_ask();
  if (!bid || !ask) {
    return 0;
  }
  return ask->price_cents - bid->price_cents;
}

const Orderbook &LocalOrderbook::state() const { return state_; }

} // namespace kalshi
