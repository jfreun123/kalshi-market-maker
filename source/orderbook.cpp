#include "orderbook.hpp"

#include <algorithm>
#include <stdexcept>

namespace kalshi {

namespace {

// Finds the iterator to the level at price_cents in a descending-sorted vector.
// Returns end() if not found.
auto find_level(std::vector<Level> &levels,
                int price_cents) -> std::vector<Level>::iterator {
  // lower_bound with descending comparator finds first element where
  // price_cents >= element, i.e. the position of price_cents.
  auto it =
      std::lower_bound(levels.begin(), levels.end(), Level{price_cents, 0},
                       [](const Level &level, const Level &target) {
                         return level.price_cents > target.price_cents;
                       });
  if (it != levels.end() && it->price_cents == price_cents) {
    return it;
  }
  return levels.end();
}

// Returns insertion point for price_cents in a descending-sorted vector
// (or the position of an existing level with that price).
auto insertion_point(std::vector<Level> &levels,
                     int price_cents) -> std::vector<Level>::iterator {
  return std::lower_bound(levels.begin(), levels.end(), Level{price_cents, 0},
                          [](const Level &level, const Level &target) {
                            return level.price_cents > target.price_cents;
                          });
}

} // namespace

void LocalOrderbook::apply_snapshot(const Orderbook &snap) { state_ = snap; }

void LocalOrderbook::apply_delta(Side side, int price_cents, int new_quantity) {
  auto &levels = (side == Side::Yes) ? state_.yes : state_.no;
  auto existing = find_level(levels, price_cents);

  if (new_quantity == 0) {
    if (existing != levels.end()) {
      levels.erase(existing);
    }
    return;
  }

  if (existing != levels.end()) {
    existing->quantity = new_quantity;
  } else {
    auto insert_pos = insertion_point(levels, price_cents);
    levels.insert(insert_pos, {price_cents, new_quantity});
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
  return (static_cast<double>(bid->price_cents) +
          static_cast<double>(ask->price_cents)) /
         2.0;
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
