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

struct QuoterConfig {
  static constexpr int kDefaultTargetSpreadCents = 4;
  static constexpr double kDefaultSkewPerContractCents = 0.05;
  static constexpr int kDefaultRepriceThresholdCents = 1;
  static constexpr int kDefaultQuoteSize = 10;

  int target_spread_cents = kDefaultTargetSpreadCents;
  double skew_per_contract_cents = kDefaultSkewPerContractCents;
  int reprice_threshold_cents = kDefaultRepriceThresholdCents;
  int quote_size = kDefaultQuoteSize;
};

// Maintains one bid (YES buy) and one ask (NO buy) per subscribed ticker.
//
// On each update() call the desired bid and ask prices are recomputed from the
// orderbook mid, a half-spread, and inventory skew. An order is replaced only
// when its price drifts by more than reprice_threshold_cents to avoid churn.
class Quoter {
public:
  Quoter(QuoterConfig config, OrderManager &order_mgr, RiskManager &risk_mgr);

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
  OrderManager &order_mgr_;
  RiskManager &risk_mgr_;
  std::unordered_map<std::string, LiveQuote> live_quotes_;
};

} // namespace kalshi
