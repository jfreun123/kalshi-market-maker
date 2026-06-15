#include "risk_manager.hpp"

#include <cstdlib>
#include <string>

namespace kalshi {

constexpr double kCentsToDollars = 100.0;

RiskManager::RiskManager(RiskLimits limits) : limits_{std::move(limits)} {}

bool RiskManager::check_order(const std::string &ticker, Side side,
                              int /*price_cents*/, int quantity) const {
  if (halted_) {
    return false;
  }
  if (quantity > limits_.max_order_size) {
    return false;
  }

  auto open_it = cached_open_order_count_.find(ticker);
  const int open_count =
      (open_it != cached_open_order_count_.end()) ? open_it->second : 0;
  if (open_count >= limits_.max_open_orders_per_market) {
    return false;
  }

  auto pos_it = cached_position_.find(ticker);
  const int current_pos =
      (pos_it != cached_position_.end()) ? pos_it->second : 0;
  const int delta = (side == Side::Yes) ? quantity : -quantity;
  if (std::abs(current_pos + delta) > limits_.max_position_per_market) {
    return false;
  }

  return true;
}

void RiskManager::update(const OrderManager &om,
                         const std::vector<std::string> &tickers) {
  cached_position_.clear();
  cached_open_order_count_.clear();
  cached_total_pnl_cents_ = 0.0;

  for (const auto &order_entry : om.open_orders()) {
    cached_open_order_count_[order_entry.second.market_ticker]++;
  }

  for (const auto &ticker : tickers) {
    cached_position_[ticker] = om.net_position(ticker);
    cached_total_pnl_cents_ += om.realized_pnl(ticker);
  }

  if (cached_total_pnl_cents_ / kCentsToDollars < limits_.daily_loss_limit) {
    halted_ = true;
  }
}

bool RiskManager::is_halted() const { return halted_; }

void RiskManager::halt() { halted_ = true; }

void RiskManager::resume() { halted_ = false; }

} // namespace kalshi
