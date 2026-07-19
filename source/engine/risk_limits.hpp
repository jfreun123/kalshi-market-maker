#pragma once
// Per-market and portfolio risk limits consumed by RiskManager. Extracted
// so config loading does not drag in the RiskManager.

namespace kalshi {

struct RiskLimits {
  static constexpr int kDefaultMaxPosition = 100;
  static constexpr int kDefaultMaxOpenOrders = 4;
  static constexpr int kDefaultMaxOrderSize = 25;
  static constexpr double kDefaultDailyLossLimit = -500.0; // dollars
  // Portfolio-wide cap on total capital at risk across all markets. Per-market
  // limits don't bound aggregate exposure at scale; this does.
  static constexpr double kDefaultMaxTotalExposure = 10000.0; // dollars
  // Portfolio-wide kill-switch on total PnL including open-inventory drawdown.
  // daily_loss_limit only watches realized PnL; this watches realized +
  // mark-to-market unrealized, so a book bleeding while holding inventory
  // halts.
  static constexpr double kDefaultMaxTotalLoss = -1000.0; // dollars
  // Price-range gate: only quote contracts priced inside [min, max] cents. The
  // low bound avoids cheap longshots — Bürgi/Deng/Whelan find maker returns on
  // <10c contracts are significantly negative; the high bound caps
  // capital-inefficient near-settled extremes. Enforced in check_order.
  static constexpr int kDefaultMinQuotePrice = 10; // cents
  static constexpr int kDefaultMaxQuotePrice = 90; // cents
  // Drawdown kill-switch: max total PnL (realized + unrealized) we may give
  // back from its session high-water mark before halting. Unlike max_total_loss
  // (anchored at break-even), this protects gains — it can fire while still net
  // profitable. Positive dollars.
  static constexpr double kDefaultMaxDrawdown = 500.0; // dollars

  // Max-hold forced exit (item 70): a position older than this is
  // force-flattened at taker prices — a bounded fee in place of unbounded z²
  // drift (ND-HFTT pattern). The passive exit path (asymmetric unwind
  // pricing, item 64) runs the whole time; this is the deadline behind it.
  // 0 disables (default until attribution tunes it).
  static constexpr int kDefaultMaxHoldSeconds = 0;

  int max_position_per_market = kDefaultMaxPosition;
  int max_hold_seconds = kDefaultMaxHoldSeconds;
  int max_open_orders_per_market = kDefaultMaxOpenOrders;
  int max_order_size = kDefaultMaxOrderSize;
  double daily_loss_limit = kDefaultDailyLossLimit; // dollars (negative = loss)
  double max_total_exposure_dollars = kDefaultMaxTotalExposure;
  double max_total_loss_dollars = kDefaultMaxTotalLoss; // dollars (negative)
  int min_quote_price_cents = kDefaultMinQuotePrice;
  int max_quote_price_cents = kDefaultMaxQuotePrice;
  double max_drawdown_dollars = kDefaultMaxDrawdown; // dollars (positive)
};

} // namespace kalshi
