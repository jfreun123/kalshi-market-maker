#pragma once
// Quoting parameters for the market-making strategy: spread, size, rest
// timers, guards, and pricing-model selection. Extracted so config loading
// does not drag in the full Quoter.

#include "engine/pricing_model.hpp"

namespace kalshi {

struct QuoterConfig {
  static constexpr int kDefaultTargetSpreadCents = 4;
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
  // Adverse theo-jump fade (Bürgi p.27: the maker edge *is* repricing). A fair
  // value move of at least theo_jump_cents against a resting order makes it
  // toxic — someone can trade it at an immediate profit — so that side may
  // cancel after only fade_rest_ms instead of the full min_rest_ms. The fade
  // floor stays above the exchange's sub-second echo of a cancelled level, so
  // the D9 oscillator class stays dead.
  static constexpr int kDefaultTheoJumpCents = 3;
  static constexpr int kDefaultFadeRestMs = 500;
  // Longshot-side edge floor (Bürgi Fig. 6 / Table 10): maker returns are
  // negative on cheap-side buys, so a quote that would buy below the threshold
  // is shaded by extra cents of edge; the favorite side quotes normally.
  static constexpr int kDefaultLongshotThresholdCents = 40;
  static constexpr int kDefaultLongshotEdgeCents = 1;
  // EMA smoothing of the micro-price fair-value anchor (item 21a / D13): raw
  // micro flaps +/-3c sub-second on in-play books and every flap passed the
  // theo-jump gate — smooth the belief, not just the action. 1.0 = unsmoothed.
  static constexpr double kDefaultFvEmaAlpha = 0.2;
  // Passive wind-down window at shutdown (item 56): exit as a maker for up
  // to this long before force-flattening the remainder. 0 = old behavior.
  static constexpr int kDefaultWinddownSeconds = 45;
  // Directional flow lean (item 32): while flow is imbalanced, shift fair
  // value toward the takers' side by this much — believe persistent flow.
  static constexpr double kDefaultFlowLeanCents = 1.0;
  // Clearing-price fv (item 66 Phase 4): tape blend weight and the VWAP
  // half-life the quoter queries the tape with. Weight only applies when the
  // ClearingPriceModel is selected (use_clearing_pricing).
  static constexpr double kDefaultClearingTapeWeight = 0.5;
  static constexpr int kDefaultTapeHalfLifeSeconds = 30;
  // Asymmetric unwind pricing (item 64): with inventory on, the reducing side
  // quotes at the reservation price plus only this much edge — closing risk
  // pays up to fair value; only opening risk charges the full half-spread.
  // Round trips complete at the first counter-flow instead of waiting out a
  // spread that double-charges.
  static constexpr double kDefaultUnwindEdgeCents = 0.0;
  // Inventory brake: at this many multiples of quote_size, the accumulating
  // side stops quoting entirely (run: a 30-lot pile-up vs 10-lot quotes).
  static constexpr int kDefaultInventoryCapLots = 2;

  int target_spread_cents = kDefaultTargetSpreadCents;
  int reprice_threshold_cents = kDefaultRepriceThresholdCents;
  int quote_size = kDefaultQuoteSize;
  int imbalance_spread_cents = kDefaultImbalanceSpreadCents;
  int min_spread_cents = kDefaultMinSpreadCents;
  int min_rest_ms = kDefaultMinRestMs;
  int theo_jump_cents = kDefaultTheoJumpCents;
  int fade_rest_ms = kDefaultFadeRestMs;
  int longshot_price_threshold_cents = kDefaultLongshotThresholdCents;
  int longshot_edge_cents = kDefaultLongshotEdgeCents;
  double fv_ema_alpha = kDefaultFvEmaAlpha;
  int winddown_seconds = kDefaultWinddownSeconds;
  double flow_lean_cents = kDefaultFlowLeanCents;
  bool use_clearing_pricing = false;
  double clearing_tape_weight = kDefaultClearingTapeWeight;
  int tape_half_life_seconds = kDefaultTapeHalfLifeSeconds;
  int inventory_cap_lots = kDefaultInventoryCapLots;
  double unwind_edge_cents = kDefaultUnwindEdgeCents;
  // Price toward the favorite-longshot-debiased view instead of the raw mid.
  // Off by default (HeuristicModel is the safe baseline); β per Bürgi et al.
  bool use_view_based_pricing = false;
  double view_debias_beta = ViewBasedModel::kDefaultBeta;
  // Maker fee rate γ; the per-contract fee is γ·P·(1−P). The quoted spread is
  // widened to cover it so net-of-fee edge stays positive. 0 = no maker fee
  // (default — set to your market's actual rate, e.g. 0.07).
  double maker_fee_rate = 0.0;
  // Drift lean (item 60a): when the DriftEstimator's slope is significant
  // (|t| >= the threshold), shift fair value by gain × slope(c/min), weighted
  // by tape confirmation (prints in window / confirm_prints, capped at 1 —
  // drift on a thin tape is only partially trusted) and clamped to ±max.
  // Gain 0 disables (default until a matched A/B validates the flip).
  static constexpr double kDefaultDriftLeanGain = 0.0;
  static constexpr double kDefaultDriftLeanMaxCents = 2.0;
  static constexpr double kDefaultDriftTStatThreshold = 2.0;
  static constexpr int kDefaultDriftConfirmPrints = 5;
  double drift_lean_gain = kDefaultDriftLeanGain;
  double drift_lean_max_cents = kDefaultDriftLeanMaxCents;
  double drift_t_stat_threshold = kDefaultDriftTStatThreshold;
  int drift_confirm_prints = kDefaultDriftConfirmPrints;
  // Panic pull tier (item 51): a catastrophic adverse jump of at least this
  // many cents cancels the toxic side instantly — no fade confirmation, no
  // rest floor — and that side stays unquoted for panic_settle_ms while the
  // book settles. 0 disables (default until Jacob confirms the tier).
  static constexpr int kDefaultPanicJumpCents = 0;
  static constexpr int kDefaultPanicSettleMs = 2'000;
  int panic_jump_cents = kDefaultPanicJumpCents;
  int panic_settle_ms = kDefaultPanicSettleMs;
};

} // namespace kalshi
