#pragma once

#include "fair_value.hpp"
#include "order_manager.hpp"
#include "orderbook.hpp"
#include "risk_manager.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace kalshi {

class FlowImbalanceGuard; // widen the spread under adverse one-sided flow

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

  int target_spread_cents = kDefaultTargetSpreadCents;
  double skew_per_contract_cents = kDefaultSkewPerContractCents;
  int reprice_threshold_cents = kDefaultRepriceThresholdCents;
  int quote_size = kDefaultQuoteSize;
  int imbalance_spread_cents = kDefaultImbalanceSpreadCents;
  int min_spread_cents = kDefaultMinSpreadCents;
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
// when its price drifts by more than reprice_threshold_cents to avoid churn.
class Quoter {
public:
  // Default pricing: HeuristicModel. flow_guard is optional (nullptr disables
  // the imbalance widening) and must outlive the Quoter.
  Quoter(QuoterConfig config, IOrderManager &order_mgr, RiskManager &risk_mgr,
         const FlowImbalanceGuard *flow_guard = nullptr);

  // Custom pricing: inject any IPricingModel via a FairValueEngine.
  Quoter(QuoterConfig config, FairValueEngine fv_engine,
         IOrderManager &order_mgr, RiskManager &risk_mgr,
         const FlowImbalanceGuard *flow_guard = nullptr);

  void update(std::string_view ticker, const LocalOrderbook &book);

private:
  struct LiveQuote {
    std::string bid_order_id;
    std::string ask_order_id;
    int current_bid_cents{0};
    int current_ask_cents{
        0}; // stored as YES ask price (not the NO order price)
  };

  // Returns {bid_cents, ask_cents} with bid ∈ [1,98] and ask ∈ [2,99].
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  static std::pair<int, int> compute_quotes(double fv_cents, int half_spread,
                                            double inventory_skew_cents);

  void refresh_bid(const std::string &ticker, int desired_bid);
  void refresh_ask(const std::string &ticker, int desired_ask);

  QuoterConfig config_;
  FairValueEngine fv_engine_;
  IOrderManager &order_mgr_;
  RiskManager &risk_mgr_;
  const FlowImbalanceGuard *flow_guard_{nullptr};
  std::unordered_map<std::string, LiveQuote> live_quotes_;
};

} // namespace kalshi
