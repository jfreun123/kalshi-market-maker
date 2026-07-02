#include "risk_manager.hpp"

#include "logger.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace kalshi {

constexpr double kCentsToDollars = 100.0;

// Human-readable names for each constraint bit — indexed by Constraint value.
constexpr std::array<std::string_view, 11> kConstraintNames = {
    "kPnLLimit",     "kPositionLimit", "kOpenOrders", "kHighFillRate",
    "kStaleBook",    "kModelDiverge",  "kManualHalt", "kConnectivity",
    "kOverExposure", "kPortfolioLoss", "kDrawdown",
};

RiskManager::RiskManager(RiskLimits limits) : limits_{limits} {}

// NOLINTBEGIN(bugprone-easily-swappable-parameters) — mirrors header decl
bool RiskManager::check_order(std::string_view ticker, Side side,
                              int price_cents, int quantity) const {
  if (constraints_.any()) {
    return false;
  }
  if (quantity > limits_.max_order_size) {
    return false;
  }
  // Price-range gate: refuse to quote a contract priced outside the band.
  if (price_cents < limits_.min_quote_price_cents ||
      price_cents > limits_.max_quote_price_cents) {
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
  const Quantity current_pos =
      (pos_it != cached_position_.end()) ? pos_it->second : Quantity{};
  const Quantity delta =
      Quantity::from_contracts((side == Side::Yes) ? quantity : -quantity);
  return kalshi::abs(current_pos + delta) <=
         Quantity::from_contracts(limits_.max_position_per_market);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

void RiskManager::update(const IOrderManager &order_mgr,
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
    set(Constraint::kPnLLimit);
  }
}

void RiskManager::update_portfolio(const PortfolioSnapshot &snapshot) {
  if (snapshot.total_notional_cents / kCentsToDollars >
      limits_.max_total_exposure_dollars) {
    set(Constraint::kOverExposure);
  }

  const double total_pnl_cents = snapshot.total_pnl_cents();
  if (total_pnl_cents / kCentsToDollars < limits_.max_total_loss_dollars) {
    set(Constraint::kPortfolioLoss);
  }

  // Drawdown: track the high-water mark of total PnL and halt if we have given
  // back more than the limit from it. Fires even while still net profitable.
  peak_total_pnl_cents_ = std::max(peak_total_pnl_cents_, total_pnl_cents);
  const double drawdown_cents = peak_total_pnl_cents_ - total_pnl_cents;
  if (drawdown_cents / kCentsToDollars > limits_.max_drawdown_dollars) {
    set(Constraint::kDrawdown);
  }
}

void RiskManager::set(Constraint bit) {
  const auto idx = static_cast<std::size_t>(bit);
  constraints_.set(idx);
  get_logger()->warn("constraint set name={}", kConstraintNames.at(idx));
}

void RiskManager::clear(Constraint bit) {
  const auto idx = static_cast<std::size_t>(bit);
  constraints_.reset(idx);
  get_logger()->info("constraint cleared name={}", kConstraintNames.at(idx));
}

bool RiskManager::is_set(Constraint bit) const {
  return constraints_.test(static_cast<std::size_t>(bit));
}

std::string RiskManager::active_constraints() const {
  std::string result;
  for (std::size_t idx = 0; idx < kNumConstraints; ++idx) {
    if (constraints_.test(idx)) {
      if (!result.empty()) {
        result += ' ';
      }
      result += kConstraintNames.at(idx);
    }
  }
  return result;
}

bool RiskManager::is_halted() const { return constraints_.any(); }

void RiskManager::halt() { set(Constraint::kManualHalt); }

void RiskManager::resume() {
  constraints_.reset();
  // Re-anchor the drawdown high-water mark to the next observation so a manual
  // resume doesn't immediately re-trip kDrawdown on the still-depressed PnL.
  peak_total_pnl_cents_ = std::numeric_limits<double>::lowest();
}

} // namespace kalshi
