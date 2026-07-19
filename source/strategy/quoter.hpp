#pragma once

#include "engine/fair_value.hpp"
#include "engine/order_manager_iface.hpp"
#include "engine/orderbook.hpp"
#include "engine/risk_manager.hpp"
#include "engine/strategy.hpp"
#include "strategy/quoter_config.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace kalshi {

class FlowImbalanceGuard; // widen the spread under adverse one-sided flow
class AnalyticsLogger;    // JSONL quote/fill events for offline measurement
class TradeTape;          // rolling public-print window (clearing-price fv)
class DriftEstimator;     // rolling mid-drift slope + t-stat (item 60a)

// LMSR log-odds inventory skew (Berg & Proebsting pp.49-56): the reservation
// price shifts a constant amount per contract in log-odds space, so the skew
// self-attenuates near 1c/99c and can never push a quote out of range.
// lmsr_b_from_risk calibrates the liquidity parameter b so holding
// max_position moves a 50c reservation price exactly to the quote-band edge;
// a degenerate band (edge <= 50c) returns an infinite b, disabling the skew.
[[nodiscard]] double lmsr_skewed_fair_value(double fv_cents,
                                            double net_inventory_contracts,
                                            double b_contracts);
[[nodiscard]] double lmsr_b_from_risk(int max_position_contracts,
                                      int max_quote_price_cents);

// Maintains one bid (YES buy) and one ask (NO buy) per subscribed ticker.
//
// On each update() call the desired bid and ask prices are recomputed from the
// orderbook mid, a half-spread, and inventory skew. An order is replaced only
// when its price drifts by more than reprice_threshold_cents AND it has rested
// at least min_rest_ms — both guards exist to avoid churn oscillators.
class Quoter : public IStrategy {
public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  // Default pricing: HeuristicModel. flow_guard is optional (nullptr disables
  // the imbalance widening) and must outlive the Quoter. clock defaults to
  // steady_clock::now; injected in tests to drive the rest-time hysteresis.
  Quoter(QuoterConfig config, IOrderManager &order_mgr, RiskManager &risk_mgr,
         const FlowImbalanceGuard *flow_guard = nullptr, Clock clock = {});

  // Custom pricing: inject any IPricingModel via a FairValueEngine.
  Quoter(QuoterConfig config, FairValueEngine fv_engine,
         IOrderManager &order_mgr, RiskManager &risk_mgr,
         const FlowImbalanceGuard *flow_guard = nullptr, Clock clock = {});

  void update(std::string_view ticker, const LocalOrderbook &book) override;

  // Wind-down mode (item 56): quote only the side that reduces inventory —
  // exit as a maker instead of paying the taker flatten at session end. The
  // accumulating side is cancelled; a flat market places nothing.
  void set_reduce_only(bool reduce_only) override;

  // Optional analytics tap: when set, every update() emits a quote-decision
  // event (PLAN item 31). Must outlive the Quoter; nullptr disables.
  void set_analytics(AnalyticsLogger *analytics);
  void set_trade_tape(const TradeTape *trade_tape);
  // Optional drift lean input (item 60a): the quoter feeds every update's mid
  // into the estimator and, when drift_lean_gain > 0 and the slope is
  // significant, leans fair value with it. Must outlive the Quoter.
  void set_drift_estimator(DriftEstimator *estimator);

  // Forget all tracked resting quotes. Call this after the resting orders have
  // been cancelled out-of-band (risk halt, disconnect, shutdown): the quoter's
  // tracked own-quote ids would otherwise be stale, so it would try to cancel
  // dead ids
  // and never re-quote once the feed recovers.
  void reset_quotes() override;

  // Forget a single tracked quote whose order left the book (e.g. it fully
  // filled). Without this the quoter believes the side is still quoted and
  // never re-places it — the side goes dark after its first complete fill.
  void forget_order(std::string_view ticker,
                    std::string_view order_id) override;

  // Forget every tracked quote on one market after its resting orders were
  // cancelled out-of-band (e.g. the feed-liveness gate): the quoter re-places
  // both sides on the market's next update instead of tracking dead ids.
  void forget_ticker(std::string_view ticker) override;

private:
  // Our own resting quote pair on one market: which orders are ours and the
  // price we quoted them at (what the reprice threshold compares against).
  // How much of them is still unfilled lives in IOrderManager::open_orders().
  struct OwnQuote {
    std::string bid_order_id;
    std::string ask_order_id;
    int quoted_bid_cents{0};
    int quoted_ask_cents{0}; // stored as YES ask price (not the NO order price)
    std::chrono::steady_clock::time_point bid_placed_at;
    std::chrono::steady_clock::time_point ask_placed_at;
    bool bid_fade_pending{false};
    bool ask_fade_pending{false};
  };

  // Returns {bid_cents, ask_cents} with bid ∈ [1,98] and ask ∈ [2,99].
  static std::pair<int, int> compute_quotes(double fv_cents, int half_spread);

  void refresh_bid(const std::string &ticker, OwnQuote &own, int desired_bid,
                   std::chrono::steady_clock::time_point now);
  void refresh_ask(const std::string &ticker, OwnQuote &own, int desired_ask,
                   std::chrono::steady_clock::time_point now);
  [[nodiscard]] static bool
  resting_too_young(std::chrono::steady_clock::time_point placed_at,
                    std::chrono::steady_clock::time_point now, int rest_ms);
  [[nodiscard]] bool release_order(const std::string &order_id);
  [[nodiscard]] const LocalOrderbook &
  book_without_own_quotes(const OwnQuote &own, const LocalOrderbook &book);

  QuoterConfig config_;
  FairValueEngine fv_engine_;
  IOrderManager &order_mgr_;
  RiskManager &risk_mgr_;
  const FlowImbalanceGuard *flow_guard_{nullptr};
  Clock clock_;
  double inventory_b_contracts_;
  std::unordered_map<std::string, double> fv_ema_;
  AnalyticsLogger *analytics_{nullptr};
  const TradeTape *trade_tape_{nullptr};
  DriftEstimator *drift_estimator_{nullptr};
  bool reduce_only_{false};
  std::unordered_map<std::string, OwnQuote> own_quotes_;
  LocalOrderbook scratch_book_;
};

} // namespace kalshi
