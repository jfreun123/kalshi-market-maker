#pragma once

#include "quantity.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace kalshi {

// ---- Enums ----

enum class Side : std::uint8_t { Yes, No };
enum class OrderStatus : std::uint8_t {
  Open,
  PartiallyFilled,
  Filled,
  Cancelled
};
enum class OrderType : std::uint8_t { Limit, Market };

// ---- Orderbook types ----

struct Level {
  int price_cents{0};
  Quantity quantity{};

  bool operator==(const Level &) const = default;
};

struct Orderbook {
  std::string ticker;
  std::vector<Level> yes; // sorted descending by price_cents
  std::vector<Level> no;  // sorted descending by price_cents
};

// ---- Order ----

struct Order {
  std::string id;
  std::string market_ticker;
  Side side{Side::Yes};
  int price_cents{0};
  Quantity quantity{};
  Quantity filled_quantity{};
  OrderStatus status{OrderStatus::Open};
  OrderType type{OrderType::Limit};
  std::chrono::system_clock::time_point created_at;

  [[nodiscard]] Quantity remaining_quantity() const {
    return quantity - filled_quantity;
  }

  [[nodiscard]] bool is_active() const {
    return status == OrderStatus::Open ||
           status == OrderStatus::PartiallyFilled;
  }
};

// ---- Fill ----

struct Fill {
  std::string order_id;
  std::string trade_id;
  std::string market_ticker;
  Side side{Side::Yes};
  int price_cents{0};
  Quantity quantity{};
  double fee_cents{0.0};
  bool is_taker{false};
  std::chrono::system_clock::time_point timestamp;
};

// ---- Market ----

struct Market {
  std::string ticker;
  std::string title;
  std::string category;
  std::string status;
  int fee_rate_bps{0};
  int yes_bid_cents{0};
  int yes_ask_cents{0};
  double volume_24h{0.0}; // contracts traded in the last 24h
  std::chrono::system_clock::time_point close_time;
};

// ---- MarketPosition ----

// The exchange's authoritative view of our position in one market, from
// GET /portfolio/positions. Used to reconcile against local accounting.
struct MarketPosition {
  std::string ticker;
  Quantity position{};
  double realized_pnl_cents{0.0};
  double market_exposure_cents{0.0};
  int resting_orders_count{0};
};

// ---- IncentiveProgram ----

// A Kalshi Liquidity Incentive pool for one market, from
// GET /incentive_programs. Resting top-of-book size earns a pro-rata share of
// period_reward; target_size and discount_factor describe how score is
// calibrated. Used to bias scanner ranking toward incentivized markets.
struct IncentiveProgram {
  std::string market_ticker;
  long long period_reward_centicents{0};
  Quantity target_size{};
  int discount_factor_bps{0};
};

// ---- Price helpers ----

constexpr int kMinPriceCents = 1;
constexpr int kMaxPriceCents = 99;
constexpr int kPriceBasis = 100;

// Kalshi prices are integers in [1, 99] representing cents (= probability %).
[[nodiscard]] inline bool is_valid_price(int price_cents) {
  return price_cents >= kMinPriceCents && price_cents <= kMaxPriceCents;
}

// YES price + NO price = 100 (ignoring spread).
[[nodiscard]] inline int complement_price(int price_cents) {
  return kPriceBasis - price_cents;
}

} // namespace kalshi
