#pragma once

#include "order_manager.hpp"
#include "orderbook.hpp"
#include "portfolio.hpp"
#include "quoter.hpp"
#include "risk_manager.hpp"
#include "types.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kalshi {

// Owns the domain reactions of the market-making loop: live orderbook state and
// the snapshot/delta/fill handlers that drive the quoter, order manager, risk
// manager, and portfolio read-model.
//
// Deliberately free of process/IO concerns (logging sink wiring, signals,
// sockets, file persistence) so the same wiring main() runs in production can
// be driven directly by unit tests and by replaying captured exchange sessions.
// References to the order manager, risk manager, and quoter are owned by the
// caller (they outlive the session).
class TradingSession {
public:
  using PnlMap = std::unordered_map<std::string, double>;
  using OrderbookMap = std::unordered_map<std::string, LocalOrderbook>;
  // Invoked after a fill updates carried PnL, so the host can persist it.
  using PnlListener = std::function<void(const PnlMap &)>;

  // flow_guard is optional; when present, on_fill feeds it so the Quoter can
  // widen spreads under adverse one-sided flow. Must outlive the session.
  TradingSession(std::vector<std::string> tickers, IOrderManager &order_mgr,
                 RiskManager &risk_mgr, Quoter &quoter,
                 FlowImbalanceGuard *flow_guard = nullptr);

  // ---- WebSocket event reactions ----

  // Replace a market's book from a fresh snapshot (no re-quote).
  void on_snapshot(const Orderbook &snapshot);
  // Apply an incremental book update and re-quote that market.
  void on_delta(const std::string &ticker, Side side, int price_cents, int qty);
  // Record a fill, refresh per-market risk, and notify the PnL listener.
  void on_fill(const Fill &fill);
  // Cancel all resting orders (called when the market-data feed drops).
  void on_disconnect();

  // Best-effort flatten: cancel every resting order across all tickers. Never
  // throws — safe to call from shutdown / error paths.
  void cancel_all_quotes();

  // Safety net: if risk is halted, cancel all resting quotes so a halt can't
  // leave orders on the book to fill adversely. Idempotent and cheap once flat;
  // call it once per loop iteration.
  void enforce_quote_safety();

  // Apply a seed snapshot and immediately quote the market — used to place
  // initial quotes before the live feed connects.
  void seed_orderbook(const Orderbook &snapshot);

  // ---- Periodic work ----

  // Portfolio kill-switch: build the read-model snapshot and feed it to the
  // RiskManager (over-exposure + total-loss). Halts all quoting on breach.
  void run_portfolio_risk();
  // Log the per-ticker status and the whole-book portfolio aggregate.
  void log_status() const;

  // ---- Read-model access ----

  [[nodiscard]] const OrderbookMap &orderbooks() const { return ob_map_; }
  [[nodiscard]] PortfolioSnapshot portfolio_snapshot() const;
  [[nodiscard]] const PnlMap &prior_pnl() const { return prior_pnl_; }

  // Seed realized PnL carried over from a prior session (loaded from disk).
  void set_prior_pnl(PnlMap prior_pnl) { prior_pnl_ = std::move(prior_pnl); }
  void set_pnl_listener(PnlListener listener) {
    pnl_listener_ = std::move(listener);
  }

private:
  [[nodiscard]] MarkMap build_marks() const;

  std::vector<std::string> tickers_;
  IOrderManager &order_mgr_;
  RiskManager &risk_mgr_;
  Quoter &quoter_;
  FlowImbalanceGuard *flow_guard_{nullptr};
  OrderbookMap ob_map_;
  PnlMap prior_pnl_;
  PnlListener pnl_listener_;
};

} // namespace kalshi
