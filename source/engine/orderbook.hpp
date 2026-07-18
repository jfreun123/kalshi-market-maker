#pragma once

// In-memory mirror of one Kalshi market's orderbook. The YES ask is implied by
// the NO book (best_ask_yes_cents = 100 - best_bid_no_cents). Per Kalshi's
// orderbook_delta protocol (API ref §8.4), apply_delta applies a signed
// increment to a level's resting size and removes the level once that size
// reaches <= 0.

#include "core/types.hpp"

#include <optional>

namespace kalshi {

struct DepthWeighting {
  static constexpr double kFlatDecay = 1.0;
  static constexpr int kAllLevels = 0;

  double decay_per_cent = kFlatDecay;
  int max_levels = kAllLevels;
};

class LocalOrderbook {
public:
  void apply_snapshot(const Orderbook &snap);

  void apply_delta(Side side, int price_cents, Quantity delta);

  [[nodiscard]] std::optional<Level> best_bid() const;

  [[nodiscard]] std::optional<Level> best_ask() const;

  [[nodiscard]] double mid_price_cents() const;

  // Volume-adjusted mid (micro-price): each side's price weighted by the
  // *opposite* side's size, so it leans toward the side under more pressure —
  // a better fair-value anchor than the raw mid under book imbalance. Falls
  // back to the mid when a side is empty or both sizes are zero.
  [[nodiscard]] double micro_price_cents() const;

  // Full-depth balance price (BETTER_PRICING.md §3a): the micro-price
  // generalized to the whole ladder — each side contributes its
  // volume-weighted price and total depth, optionally decayed by distance
  // from the mid (decay_per_cent < 1 tames far bot walls) or truncated to
  // the innermost max_levels (0 = all). With one level per side and flat
  // decay this reduces exactly to micro_price_cents(); an empty side falls
  // back to micro_price_cents().
  [[nodiscard]] double
  clearing_price_cents(const DepthWeighting &weighting) const;

  [[nodiscard]] int spread_cents() const;

  [[nodiscard]] const Orderbook &state() const;

private:
  Orderbook state_;
};

} // namespace kalshi
