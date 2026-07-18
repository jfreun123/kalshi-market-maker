#pragma once
// Strategy seam: the trading session drives any IStrategy with book updates
// and lifecycle notifications; the strategy places and cancels orders through
// IOrderManager. Quoter is the market-making implementation; alternative
// strategies plug in here without touching the session/execution/risk stack.

#include "engine/orderbook.hpp"

#include <string_view>

namespace kalshi {

class IStrategy {
public:
  virtual ~IStrategy() = default;

  virtual void update(std::string_view ticker, const LocalOrderbook &book) = 0;
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  virtual void forget_order(std::string_view ticker,
                            std::string_view order_id) = 0;
  virtual void forget_ticker(std::string_view ticker) = 0;
  virtual void reset_quotes() = 0;
  virtual void set_reduce_only(bool reduce_only) = 0;
};

} // namespace kalshi
