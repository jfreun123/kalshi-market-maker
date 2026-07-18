#pragma once

#include "order_manager_iface.hpp"
#include "orderbook.hpp"
#include "portfolio.hpp"
#include "risk_manager.hpp"
#include "strategy.hpp"
#include "types.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kalshi {

class AnalyticsLogger;
class FlowImbalanceGuard;
class TradeTape;

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
      RiskManager &risk_mgr, IStrategy &strategy,
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
  // Feed a public print to the trade tape (no-op without a tape).
  void on_trade(const PublicTrade &trade);

  // Optional analytics tap: when set, every recorded fill emits a fill event
  // enriched with the book mid and post-fill inventory (PLAN item 31). Must
  // outlive the session; nullptr disables.
  void set_analytics(AnalyticsLogger *analytics);
  // Optional live tape: when set, on_trade records public prints and on_fill
  // marks our own trade_ids so the tape never counts our own fills. Must
  // outlive the session; nullptr disables.
  void set_trade_tape(TradeTape *trade_tape);
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

  // Recovery net for quiet books (item 48 / run-6 finding): quoting normally
  // happens on WS deltas, so a market whose seed placement failed — or whose
  // orders vanished for any transient reason — stays quoteless until the next
  // delta, which on a dormant book may never come. Re-runs the quoter for
  // every tracked market that has a live book and no resting orders. Skipped
  // while halted; respects the per-ticker error cooldown; no-op for markets
  // that are already quoted. Call on a periodic poll cadence.
  void requote_idle_markets();

  // Apply a seed snapshot and immediately quote the market — used to place
  // initial quotes before the live feed connects.
  void seed_orderbook(const Orderbook &snapshot);

  // ---- Market rotation (item 52) ----
  // Flow moves around the exchange faster than session boundaries; these let
  // the host rotate the tracked set mid-session. Removal is refused while the
  // market holds a position or resting orders — never rotate away exposure.
  void add_market(const Orderbook &snapshot);
  [[nodiscard]] bool remove_market_if_idle(const std::string &ticker);
  [[nodiscard]] bool is_tracked(std::string_view ticker) const;
  [[nodiscard]] const std::vector<std::string> &tickers() const {
    return tickers_;
  }

  // ---- Periodic work ----

  // Portfolio kill-switch: build the read-model snapshot and feed it to the
  // RiskManager (over-exposure + total-loss). Halts all quoting on breach.
  void run_portfolio_risk();
  // Log the per-ticker status and the whole-book portfolio aggregate.
  void log_status() const;

  // ---- Feed liveness ----
  // A healthy subscription always delivers at least its initial WS snapshot
  // within seconds. A market whose book has NEVER ticked past this grace has
  // no live feed (e.g. a silently failed subscribe): its resting quotes are
  // stale free options and are cancelled until a WS message arrives.
  static constexpr std::chrono::seconds kFeedConfirmGrace{30};

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
  [[nodiscard]] bool feed_confirmed(const std::string &ticker) const;

  std::vector<std::string> tickers_;
  IOrderManager &order_mgr_;
  RiskManager &risk_mgr_;
  IStrategy &strategy_;
  FlowImbalanceGuard *flow_guard_{nullptr};
  AnalyticsLogger *analytics_{nullptr};
  TradeTape *trade_tape_{nullptr};
  Clock clock_;
  std::chrono::milliseconds error_cooldown_{kDefaultErrorCooldown};
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      cooldown_until_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      last_book_update_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      seeded_at_;
  std::unordered_set<std::string> feed_dead_;
  OrderbookMap ob_map_;
  PnlMap prior_pnl_;
  PnlListener pnl_listener_;
};

} // namespace kalshi
