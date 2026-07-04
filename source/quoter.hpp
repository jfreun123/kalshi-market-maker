#pragma once

#include "fair_value.hpp"
#include "order_manager.hpp"
#include "orderbook.hpp"
#include "risk_manager.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace kalshi {

class FlowImbalanceGuard; // widen the spread under adverse one-sided flow
class AnalyticsLogger;    // JSONL quote/fill events for offline measurement

struct QuoterConfig {
  static constexpr int kDefaultTargetSpreadCents = 4;
  static constexpr double kDefaultSkewPerContractCents = 0.05;
  static constexpr int kDefaultRepriceThresholdCents = 1;
  static constexpr int kDefaultQuoteSize = 10;
  // Extra cents added to the target spread while flow is imbalanced.
  static constexpr int kDefaultImbalanceSpreadCents = 2;
  // Hard floor on the quoted spread — never quote tighter than this, so the
  // underwriting premium isn't given away (Palumbo: LPs are underwriters).
  static constexpr int kDefaultMinSpreadCents = 3;
  // Minimum time a quote must rest before the reprice branch may cancel it.
  // Kills time-domain self-reference oscillators (demo finding D9): the
  // exchange's echo of a cancelled level clears well within this window, and
  // healthy repricing (run 3: one reprice per 2.5–12s) is barely delayed.
  static constexpr int kDefaultMinRestMs = 3'000;

  int target_spread_cents = kDefaultTargetSpreadCents;
  double skew_per_contract_cents = kDefaultSkewPerContractCents;
  int reprice_threshold_cents = kDefaultRepriceThresholdCents;
  int quote_size = kDefaultQuoteSize;
  int imbalance_spread_cents = kDefaultImbalanceSpreadCents;
  int min_spread_cents = kDefaultMinSpreadCents;
  int min_rest_ms = kDefaultMinRestMs;
  // Price toward the favorite-longshot-debiased view instead of the raw mid.
  // Off by default (HeuristicModel is the safe baseline); β per Bürgi et al.
  bool use_view_based_pricing = false;
  double view_debias_beta = ViewBasedModel::kDefaultBeta;
  // Maker fee rate γ; the per-contract fee is γ·P·(1−P). The quoted spread is
  // widened to cover it so net-of-fee edge stays positive. 0 = no maker fee
  // (default — set to your market's actual rate, e.g. 0.07).
  double maker_fee_rate = 0.0;
};

// Maintains one bid (YES buy) and one ask (NO buy) per subscribed ticker.
//
// On each update() call the desired bid and ask prices are recomputed from the
// orderbook mid, a half-spread, and inventory skew. An order is replaced only
// when its price drifts by more than reprice_threshold_cents AND it has rested
// at least min_rest_ms — both guards exist to avoid churn oscillators.
class Quoter {
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

  void update(std::string_view ticker, const LocalOrderbook &book);

  // Optional analytics tap: when set, every update() emits a quote-decision
  // event (PLAN item 31). Must outlive the Quoter; nullptr disables.
  void set_analytics(AnalyticsLogger *analytics);

  // Forget all tracked resting quotes. Call this after the resting orders have
  // been cancelled out-of-band (risk halt, disconnect, shutdown): the quoter's
  // tracked own-quote ids would otherwise be stale, so it would try to cancel
  // dead ids
  // and never re-quote once the feed recovers.
  void reset_quotes();

  // Forget a single tracked quote whose order left the book (e.g. it fully
  // filled). Without this the quoter believes the side is still quoted and
  // never re-places it — the side goes dark after its first complete fill.
  void forget_order(std::string_view ticker, std::string_view order_id);

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
  };

  // Returns {bid_cents, ask_cents} with bid ∈ [1,98] and ask ∈ [2,99].
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  static std::pair<int, int> compute_quotes(double fv_cents, int half_spread,
                                            double inventory_skew_cents);

  void refresh_bid(const std::string &ticker, OwnQuote &own, int desired_bid,
                   std::chrono::steady_clock::time_point now);
  void refresh_ask(const std::string &ticker, OwnQuote &own, int desired_ask,
                   std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool
  resting_too_young(std::chrono::steady_clock::time_point placed_at,
                    std::chrono::steady_clock::time_point now) const;
  [[nodiscard]] bool release_order(const std::string &order_id);
  [[nodiscard]] const LocalOrderbook &
  book_without_own_quotes(const OwnQuote &own, const LocalOrderbook &book);

  QuoterConfig config_;
  FairValueEngine fv_engine_;
  IOrderManager &order_mgr_;
  RiskManager &risk_mgr_;
  const FlowImbalanceGuard *flow_guard_{nullptr};
  Clock clock_;
  AnalyticsLogger *analytics_{nullptr};
  std::unordered_map<std::string, OwnQuote> own_quotes_;
  LocalOrderbook scratch_book_;
};

} // namespace kalshi
