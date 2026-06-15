#include "risk_manager.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

namespace kalshi {

constexpr double kCentsToDollars = 100.0;

RiskManager::RiskManager(RiskLimits limits) : limits_{limits} {}

bool RiskManager::check_order(std::string_view ticker, Side side,
                              int /*price_cents*/, int quantity) const {
  if (halted_) {
    return false;
  }
  if (quantity > limits_.max_order_size) {
    return false;
  }

  const std::string ticker_str{ticker};
  auto open_it = cached_open_order_count_.find(ticker_str);
  const int open_count =
      (open_it != cached_open_order_count_.end()) ? open_it->second : 0;
  if (open_count >= limits_.max_open_orders_per_market) {
    return false;
  }

  auto pos_it = cached_position_.find(ticker_str);
  const int current_pos =
      (pos_it != cached_position_.end()) ? pos_it->second : 0;
  const int delta = (side == Side::Yes) ? quantity : -quantity;
  return std::abs(current_pos + delta) <= limits_.max_position_per_market;
}

void RiskManager::update(const OrderManager &order_mgr,
                         const std::vector<std::string> &tickers) {
  cached_position_.clear();
  cached_open_order_count_.clear();
  cached_total_pnl_cents_ = 0.0;

  for (const auto &order_entry : order_mgr.open_orders()) {
    cached_open_order_count_[order_entry.second.market_ticker]++;
  }

  for (const auto &ticker : tickers) {
    cached_position_[ticker] = order_mgr.net_position(ticker);
    cached_total_pnl_cents_ += order_mgr.realized_pnl(ticker);
  }

  if (cached_total_pnl_cents_ / kCentsToDollars < limits_.daily_loss_limit) {
    halted_ = true;
  }
}

bool RiskManager::is_halted() const { return halted_; }

void RiskManager::halt() { halted_ = true; }

void RiskManager::resume() { halted_ = false; }

} // namespace kalshi
