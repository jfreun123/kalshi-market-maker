#include "orderbook.hpp"

#include <algorithm>
#include <stdexcept>

namespace kalshi {

namespace {

auto find_level(std::vector<Level> &levels, int price_cents)
    -> std::vector<Level>::iterator {
  auto level_pos =
      std::lower_bound(levels.begin(), levels.end(), Level{price_cents, 0},
                       [](const Level &level, const Level &target) {
                         return level.price_cents > target.price_cents;
                       });
  if (level_pos != levels.end() && level_pos->price_cents == price_cents) {
    return level_pos;
  }
  return levels.end();
}

auto insertion_point(std::vector<Level> &levels, int price_cents)
    -> std::vector<Level>::iterator {
  return std::lower_bound(levels.begin(), levels.end(), Level{price_cents, 0},
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

void LocalOrderbook::apply_delta(Side side, int price_cents, int delta) {
  auto &levels = (side == Side::Yes) ? state_.yes : state_.no;
  auto existing = find_level(levels, price_cents);

  if (existing != levels.end()) {
    existing->quantity += delta;
    if (existing->quantity <= 0) {
      levels.erase(existing);
    }
    return;
  }

  if (delta > 0) {
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
