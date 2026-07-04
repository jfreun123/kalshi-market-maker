#pragma once

#include "order_manager.hpp"
#include "orderbook.hpp"
#include "portfolio.hpp"
#include "quoter.hpp"
#include "risk_manager.hpp"
#include "types.hpp"

#include <chrono>
#include <functional>
#include <optional>
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
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  // After a place fails (e.g. post-only cross, or a 429), the offending ticker
  // is skipped for this long instead of re-quoting the same crossing price on
  // every subsequent delta — which otherwise spins into a reject/rate-limit
  // hot loop on a fast book.
  static constexpr std::chrono::milliseconds kDefaultErrorCooldown{500};

  // flow_guard is optional; when present, on_fill feeds it so the Quoter can
  // widen spreads under adverse one-sided flow. Must outlive the session.
  // clock defaults to steady_clock::now; injected in tests to drive cooldowns.
  TradingSession(
      std::vector<std::string> tickers, IOrderManager &order_mgr,
      RiskManager &risk_mgr, Quoter &quoter,
      FlowImbalanceGuard *flow_guard = nullptr, Clock clock = {},
      std::chrono::milliseconds error_cooldown = kDefaultErrorCooldown);

  // ---- WebSocket event reactions ----

  // Replace a market's book from a fresh snapshot (no re-quote).
  void on_snapshot(const Orderbook &snapshot);
  // Apply an incremental book update and re-quote that market.
  void on_delta(const std::string &ticker, Side side, int price_cents,
                Quantity qty);
  // Record a fill, refresh per-market risk, and notify the PnL listener.
  void on_fill(const Fill &fill);
  // Record a shutdown-flatten execution so its realized PnL reaches the
  // OrderManager and the persisted carry (the WS feed is already down when the
  // flatten runs, so no fill message will ever arrive for it).
  void record_flatten(const Order &order);
  // Cancel all resting orders (called when the market-data feed drops).
  void on_disconnect();

  // Best-effort flatten: cancel every resting order across all tickers. Never
  // throws — safe to call from shutdown / error paths.
  void cancel_all_quotes();

  void cancel_preexisting_orders(const std::vector<Order> &resting_orders);

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
  // Time since the last snapshot/delta for this ticker; nullopt before the
  // first book update. Distinguishes idle-because-quiet from wedged.
  [[nodiscard]] std::optional<std::chrono::seconds>
  book_age(const std::string &ticker) const;
  [[nodiscard]] PortfolioSnapshot portfolio_snapshot() const;
  [[nodiscard]] const PnlMap &prior_pnl() const { return prior_pnl_; }

  // Prior-session PnL plus the current session's cumulative realized PnL per
  // tracked ticker — the value persisted to disk. prior_pnl_ itself is never
  // mutated during a session; folding the running total back into it would
  // double-count every earlier fill on the next one.
  [[nodiscard]] PnlMap carried_totals() const;

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
  Clock clock_;
  std::chrono::milliseconds error_cooldown_{kDefaultErrorCooldown};
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      cooldown_until_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      last_book_update_;
  OrderbookMap ob_map_;
  PnlMap prior_pnl_;
  PnlListener pnl_listener_;
};

} // namespace kalshi
