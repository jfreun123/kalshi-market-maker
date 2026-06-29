#include "trading_session.hpp"

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
                               Quoter &quoter)
    : tickers_{std::move(tickers)}, order_mgr_{order_mgr}, risk_mgr_{risk_mgr},
      quoter_{quoter} {}

void TradingSession::on_snapshot(const Orderbook &snapshot) {
  ob_map_[snapshot.ticker].apply_snapshot(snapshot);
  get_logger()->info("snapshot ticker={} yes_levels={} no_levels={}",
                     snapshot.ticker, snapshot.yes.size(), snapshot.no.size());
}

void TradingSession::on_delta(const std::string &ticker, Side side,
                              int price_cents, int qty) {
  auto &book = ob_map_[ticker];
  book.apply_delta(side, price_cents, qty);
  get_logger()->debug("delta ticker={} side={} price={} qty={} mid={:.1f}",
                      ticker, side_name(side), price_cents, qty,
                      book.mid_price_cents());
  try {
    quoter_.update(ticker, book);
  } catch (const std::exception &ex) {
    get_logger()->error("quoter error ticker={}: {}", ticker, ex.what());
  }
}

void TradingSession::on_fill(const Fill &fill) {
  order_mgr_.record_fill(fill);
  risk_mgr_.update(order_mgr_, tickers_);

  const double session_pnl = order_mgr_.realized_pnl(fill.market_ticker);
  const double total_pnl =
      prior_pnl_for(prior_pnl_, fill.market_ticker) + session_pnl;

  get_logger()->info("fill ticker={} side={} price={} qty={} is_taker={} "
                     "session_pnl=${:.2f} total_pnl=${:.2f}",
                     fill.market_ticker, side_name(fill.side), fill.price_cents,
                     fill.quantity, fill.is_taker,
                     session_pnl / kCentsPerDollar,
                     total_pnl / kCentsPerDollar);

  if (risk_mgr_.is_halted()) {
    get_logger()->critical("risk halted constraints={}",
                           risk_mgr_.active_constraints());
  }

  prior_pnl_[fill.market_ticker] = total_pnl;
  if (pnl_listener_) {
    pnl_listener_(prior_pnl_);
  }
}

void TradingSession::on_disconnect() {
  get_logger()->warn("ws disconnected — cancelling all open orders");
  cancel_all_quotes();
}

void TradingSession::cancel_all_quotes() {
  if (order_mgr_.open_orders().empty()) {
    return;
  }
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

void TradingSession::enforce_quote_safety() {
  if (risk_mgr_.is_halted()) {
    cancel_all_quotes();
  }
}

void TradingSession::seed_orderbook(const Orderbook &snapshot) {
  get_logger()->info("seeding orderbook ticker={}", snapshot.ticker);
  auto &book = ob_map_[snapshot.ticker];
  book.apply_snapshot(snapshot);
  quoter_.update(snapshot.ticker, book);
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
    const int pos = order_mgr_.net_position(ticker);
    const double session_pnl = order_mgr_.realized_pnl(ticker);
    const double prior = prior_pnl_for(prior_pnl_, ticker);
    log->info("status ticker={} net_pos={} session_pnl_dollars={:.2f} "
              "total_pnl_dollars={:.2f} halted={} constraints={}",
              ticker, pos, session_pnl / kCentsPerDollar,
              (prior + session_pnl) / kCentsPerDollar, risk_mgr_.is_halted(),
              risk_mgr_.active_constraints());
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
