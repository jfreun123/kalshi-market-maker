#include "trading_session.hpp"

#include "flow_imbalance.hpp"
#include "logger.hpp"
#include "portfolio.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace kalshi {

namespace {

constexpr double kCentsPerDollar = 100.0;
constexpr std::size_t kMaxEventsLogged = 5U;

const char *side_name(Side side) { return side == Side::Yes ? "yes" : "no"; }

double prior_pnl_for(const TradingSession::PnlMap &prior_pnl,
                     const std::string &ticker) {
  return prior_pnl.contains(ticker) ? prior_pnl.at(ticker) : 0.0;
}

} // namespace

TradingSession::TradingSession(std::vector<std::string> tickers,
                               IOrderManager &order_mgr, RiskManager &risk_mgr,
                               Quoter &quoter, FlowImbalanceGuard *flow_guard,
                               Clock clock,
                               std::chrono::milliseconds error_cooldown)
    : tickers_{std::move(tickers)}, order_mgr_{order_mgr}, risk_mgr_{risk_mgr},
      quoter_{quoter}, flow_guard_{flow_guard},
      clock_{clock ? std::move(clock)
                   : Clock{[] { return std::chrono::steady_clock::now(); }}},
      error_cooldown_{error_cooldown} {}

void TradingSession::on_snapshot(const Orderbook &snapshot) {
  ob_map_[snapshot.ticker].apply_snapshot(snapshot);
  get_logger()->info("snapshot ticker={} yes_levels={} no_levels={}",
                     snapshot.ticker, snapshot.yes.size(), snapshot.no.size());
}

void TradingSession::on_delta(const std::string &ticker, Side side,
                              int price_cents, Quantity qty) {
  auto &book = ob_map_[ticker];
  book.apply_delta(side, price_cents, qty);
  get_logger()->debug("delta ticker={} side={} price={} qty={} mid={:.1f}",
                      ticker, side_name(side), price_cents, qty.to_fp_string(),
                      book.mid_price_cents());

  const auto cooldown = cooldown_until_.find(ticker);
  if (cooldown != cooldown_until_.end() && clock_() < cooldown->second) {
    return;
  }

  try {
    quoter_.update(ticker, book);
  } catch (const std::exception &ex) {
    cooldown_until_[ticker] = clock_() + error_cooldown_;
    get_logger()->error("quoter error ticker={}: {} — cooling down {}ms",
                        ticker, ex.what(), error_cooldown_.count());
  }
}

void TradingSession::on_fill(const Fill &fill) {
  order_mgr_.record_fill(fill);
  if (!order_mgr_.open_orders().contains(fill.order_id)) {
    quoter_.forget_order(fill.market_ticker, fill.order_id);
  }
  risk_mgr_.update(order_mgr_, tickers_);
  if (flow_guard_ != nullptr) {
    flow_guard_->record_fill(fill.market_ticker, fill.side, fill.quantity,
                             fill.timestamp);
  }

  const double session_pnl = order_mgr_.realized_pnl(fill.market_ticker);
  const double total_pnl =
      prior_pnl_for(prior_pnl_, fill.market_ticker) + session_pnl;

  get_logger()->info("fill ticker={} side={} price={} qty={} is_taker={} "
                     "session_pnl=${:.2f} total_pnl=${:.2f}",
                     fill.market_ticker, side_name(fill.side), fill.price_cents,
                     fill.quantity.to_fp_string(), fill.is_taker,
                     session_pnl / kCentsPerDollar,
                     total_pnl / kCentsPerDollar);

  if (risk_mgr_.is_halted()) {
    get_logger()->critical("risk halted constraints={}",
                           risk_mgr_.active_constraints());
  }

  if (pnl_listener_) {
    pnl_listener_(carried_totals());
  }
}

void TradingSession::record_flatten(const Order &order) {
  if (!order.filled_quantity.is_positive()) {
    return;
  }
  Fill fill;
  fill.order_id = order.id;
  fill.market_ticker = order.market_ticker;
  fill.side = order.side;
  fill.price_cents = (order.average_fill_price_cents != 0)
                         ? order.average_fill_price_cents
                         : order.price_cents;
  fill.quantity = order.filled_quantity;
  fill.timestamp = order.created_at;
  fill.is_taker = true;
  order_mgr_.record_fill(fill);

  const double session_pnl = order_mgr_.realized_pnl(order.market_ticker);
  const double total_pnl =
      prior_pnl_for(prior_pnl_, order.market_ticker) + session_pnl;
  get_logger()->info(
      "flatten fill ticker={} side={} price={} qty={} session_pnl=${:.2f} "
      "total_pnl=${:.2f}",
      order.market_ticker, side_name(order.side), fill.price_cents,
      fill.quantity.to_fp_string(), session_pnl / kCentsPerDollar,
      total_pnl / kCentsPerDollar);

  if (pnl_listener_) {
    pnl_listener_(carried_totals());
  }
}

TradingSession::PnlMap TradingSession::carried_totals() const {
  PnlMap totals = prior_pnl_;
  for (const auto &ticker : tickers_) {
    const double session_pnl = order_mgr_.realized_pnl(ticker);
    if (session_pnl != 0.0 || totals.contains(ticker)) {
      totals[ticker] = prior_pnl_for(prior_pnl_, ticker) + session_pnl;
    }
  }
  return totals;
}

void TradingSession::on_disconnect() {
  get_logger()->warn("ws disconnected — cancelling all open orders");
  cancel_all_quotes();
}

void TradingSession::cancel_all_quotes() {
  if (!order_mgr_.open_orders().empty()) {
    get_logger()->warn("flattening — cancelling all resting quotes ({} open)",
                       order_mgr_.open_orders().size());
    for (const auto &ticker : tickers_) {
      // OrderManager::cancel_all is itself best-effort; the extra guard keeps a
      // surprise from one ticker from skipping the others.
      try {
        order_mgr_.cancel_all(ticker);
      } catch (const std::exception &ex) {
        get_logger()->error("cancel_all_quotes: ticker={} failed: {}", ticker,
                            ex.what());
      }
    }
  }
  // Resync the quoter to "no resting quotes". Its live-order ids point at the
  // orders we just cancelled; without this it would try to cancel dead ids and
  // never re-quote when the feed recovers.
  quoter_.reset_quotes();
}

void TradingSession::enforce_quote_safety() {
  if (risk_mgr_.is_halted()) {
    cancel_all_quotes();
  }
}

void TradingSession::cancel_preexisting_orders(
    const std::vector<Order> &resting_orders) {
  int cancelled = 0;
  int left_untracked = 0;
  for (const auto &order : resting_orders) {
    const bool tracked = std::find(tickers_.begin(), tickers_.end(),
                                   order.market_ticker) != tickers_.end();
    if (!tracked) {
      get_logger()->warn(
          "startup: resting order on untracked ticker={} id={} left in place",
          order.market_ticker, order.id);
      ++left_untracked;
      continue;
    }
    try {
      get_logger()->warn(
          "startup: cancelling pre-existing order ticker={} id={}",
          order.market_ticker, order.id);
      order_mgr_.cancel(order.id);
      ++cancelled;
    } catch (const std::exception &ex) {
      get_logger()->error("startup: failed to cancel orphan id={}: {}",
                          order.id, ex.what());
    }
  }
  if (cancelled > 0 || left_untracked > 0) {
    get_logger()->info(
        "startup: cancelled {} orphan order(s) on tracked tickers; left {} on "
        "untracked tickers",
        cancelled, left_untracked);
  }
}

void TradingSession::seed_orderbook(const Orderbook &snapshot) {
  get_logger()->info("seeding orderbook ticker={}", snapshot.ticker);
  auto &book = ob_map_[snapshot.ticker];
  book.apply_snapshot(snapshot);
  // Contain quoting failures (e.g. the exchange rejecting an initial quote that
  // would cross a tight book): seeding one market must never abort startup. The
  // book is still recorded, so the live feed can quote it later.
  try {
    quoter_.update(snapshot.ticker, book);
  } catch (const std::exception &ex) {
    get_logger()->error("seed quote error ticker={}: {}", snapshot.ticker,
                        ex.what());
  }
}

MarkMap TradingSession::build_marks() const {
  MarkMap marks;
  for (const auto &ticker : tickers_) {
    auto book_it = ob_map_.find(ticker);
    if (book_it == ob_map_.end()) {
      continue;
    }
    const double mid = book_it->second.mid_price_cents();
    if (mid > 0.0) {
      marks[ticker] = static_cast<int>(std::lround(mid));
    }
  }
  return marks;
}

PortfolioSnapshot TradingSession::portfolio_snapshot() const {
  const Portfolio portfolio{order_mgr_};
  return portfolio.snapshot(tickers_, build_marks());
}

void TradingSession::run_portfolio_risk() {
  const bool was_halted = risk_mgr_.is_halted();
  const auto snap = portfolio_snapshot();
  risk_mgr_.update_portfolio(snap);
  if (risk_mgr_.is_halted() && !was_halted) {
    get_logger()->critical(
        "portfolio risk halt — constraints={} total_pnl_dollars={:.2f} "
        "capital_at_risk_dollars={:.2f}",
        risk_mgr_.active_constraints(),
        snap.total_pnl_cents() / kCentsPerDollar,
        snap.total_notional_cents / kCentsPerDollar);
  }
}

void TradingSession::log_status() const {
  auto log = get_logger();
  for (const auto &ticker : tickers_) {
    const Quantity pos = order_mgr_.net_position(ticker);
    const double session_pnl = order_mgr_.realized_pnl(ticker);
    const double fees = order_mgr_.fees_paid(ticker);
    const double prior = prior_pnl_for(prior_pnl_, ticker);
    log->info("status ticker={} net_pos={} session_pnl_dollars={:.2f} "
              "fees_dollars={:.2f} total_pnl_dollars={:.2f} halted={} "
              "constraints={}",
              ticker, pos.to_fp_string(), session_pnl / kCentsPerDollar,
              fees / kCentsPerDollar, (prior + session_pnl) / kCentsPerDollar,
              risk_mgr_.is_halted(), risk_mgr_.active_constraints());

    // E_win decomposition: locked spread capture vs. the directional bet.
    const auto exposure = order_mgr_.exposure_decomposition(ticker);
    log->info(
        "exposure ticker={} net_inventory={} spread_capture_dollars={:.2f} "
        "e_win_dollars={:.2f} e_loss_dollars={:.2f}",
        ticker, exposure.net_inventory.to_fp_string(),
        exposure.spread_capture_cents / kCentsPerDollar,
        exposure.e_win_cents / kCentsPerDollar,
        exposure.e_loss_cents / kCentsPerDollar);
  }

  const auto snap = portfolio_snapshot();
  log->info("portfolio realized_dollars={:.2f} unrealized_dollars={:.2f} "
            "total_pnl_dollars={:.2f} capital_at_risk_dollars={:.2f} events={}",
            snap.total_realized_cents / kCentsPerDollar,
            snap.total_unrealized_cents / kCentsPerDollar,
            snap.total_pnl_cents() / kCentsPerDollar,
            snap.total_notional_cents / kCentsPerDollar, snap.by_event.size());

  const std::size_t shown = std::min(kMaxEventsLogged, snap.by_event.size());
  for (std::size_t idx = 0U; idx < shown; ++idx) {
    const auto &event = snap.by_event[idx];
    log->info("  event={} markets={} pnl_dollars={:.2f} "
              "capital_at_risk_dollars={:.2f}",
              event.event_ticker, event.market_count,
              event.total_pnl_cents() / kCentsPerDollar,
              event.notional_cost_cents / kCentsPerDollar);
  }
}

} // namespace kalshi
