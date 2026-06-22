#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace kalshi {

// ---- Enums ----

enum class Side { Yes, No };
enum class OrderStatus { Open, PartiallyFilled, Filled, Cancelled };
enum class OrderType { Limit, Market };

// ---- Orderbook types ----

struct Level {
  int price_cents{0};
  int quantity{0};

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
  int quantity{0};
  int filled_quantity{0};
  OrderStatus status{OrderStatus::Open};
  OrderType type{OrderType::Limit};
  std::chrono::system_clock::time_point created_at;

  [[nodiscard]] int remaining_quantity() const {
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
  std::string market_ticker;
  Side side{Side::Yes};
  int price_cents{0};
  int quantity{0};
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
  double volume_usd{0.0};
  std::chrono::system_clock::time_point close_time;
};

// ---- Price helpers ----

// Kalshi prices are integers in [1, 99] representing cents (= probability %).
[[nodiscard]] inline bool is_valid_price(int price_cents) {
  return price_cents >= 1 && price_cents <= 99;
}

// YES price + NO price = 100 (ignoring spread).
[[nodiscard]] inline int complement_price(int price_cents) {
  return 100 - price_cents;
}

} // namespace kalshi
