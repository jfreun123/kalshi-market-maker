#pragma once

#include "types.hpp"

#include <optional>

namespace kalshi {

// In-memory mirror of one Kalshi market's orderbook.
//
// Kalshi YES/NO mechanics: the YES ask is implied by the NO book.
//   best_ask_yes_cents = 100 - best_bid_no_cents
//
// Delta protocol: apply_delta with new_quantity=0 removes the level.
class LocalOrderbook {
public:
  void apply_snapshot(const Orderbook &snap);

  // Upserts or removes one level. new_quantity=0 removes the level.
  void apply_delta(Side side, int price_cents, int new_quantity);

  // Best bid = highest YES price. nullopt if YES book is empty.
  [[nodiscard]] std::optional<Level> best_bid() const;

  // Best ask = 100 - highest NO price. nullopt if NO book is empty.
  [[nodiscard]] std::optional<Level> best_ask() const;

  // Average of best bid and best ask in cents. Returns 0 if either side empty.
  [[nodiscard]] double mid_price_cents() const;

  // ask - bid in cents. Returns 0 if either side empty.
  [[nodiscard]] int spread_cents() const;

  [[nodiscard]] const Orderbook &state() const;

private:
  Orderbook state_;
};

} // namespace kalshi
