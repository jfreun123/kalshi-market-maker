#pragma once
// Market-scanner filters and gates. Extracted so config loading does not
// drag in the scanner and its REST client dependency.

#include <string>
#include <vector>

namespace kalshi {

struct ScannerConfig {
  static constexpr int kDefaultMinPriceCents = 15;
  static constexpr int kDefaultMaxPriceCents = 85;
  static constexpr int kDefaultMinSpreadCents = 3;
  static constexpr int kDefaultMaxSpreadCents = 10;
  static constexpr double kDefaultMinVolume24h = 1000.0;
  static constexpr double kDefaultMinDaysToClose = 1.0;
  static constexpr double kDefaultMaxDaysToClose = 90.0;
  // Additive score bonus (on top of the [0,1] volume+spread score) for markets
  // with an active liquidity incentive pool. Rebates can dominate thin spreads,
  // so this is a strong but not overwhelming nudge.
  static constexpr double kDefaultIncentiveWeight = 0.50;
  // Number of top-ranked tickers written into the generated trade config.
  // PLAN.md notes the Basic rate tier is safe at <=5 concurrent tickers.
  static constexpr int kDefaultTradeTopN = 5;
  // Liveness filter (item 49): vol_24h is up to a day stale, so a market that
  // was busy this morning can be dormant now. Ranked candidates whose most
  // recent public trade is older than this are dropped (0 disables; unknown
  // last-trade counts as fresh).
  static constexpr int kDefaultMaxStaleTradeMinutes = 30;
  static constexpr double kDefaultMinMinorityFlowRatio = 0.0;
  static constexpr double kDefaultMinReversionKappa = 0.0;
  static constexpr int kDefaultReversionWindowMinutes = 180;
  // In-session market rotation cadence (item 52): re-scan and swap dead-idle
  // markets for live ones every this many minutes; 0 disables rotation.
  static constexpr int kDefaultRotationMinutes = 5;
  // Flow-rate admission (item 61): finalists need at least this many public
  // trades within the past hour — vol_24h credits yesterday's burst, this
  // demands takers NOW. A parsed-but-empty trade tape is a definitive drop;
  // probe errors fail open. 0 disables.
  static constexpr int kDefaultMinTradesPerHour = 6;
  // Tape-range admission (item 62): recent prints must span at least this
  // many cents. A tape pinned at one price is a determined market (expired
  // event, settlement lottery) — trades exist but price discovery is over,
  // and only informed takers remain. 0 disables.
  static constexpr int kDefaultMinTradePriceRangeCents = 2;
  // How far back the tape-range check looks (item 63): pre-game markets
  // print sparsely, so an hour of quiet is normal while the morning's steps
  // are real price discovery. Determined markets stay pinned across any
  // window.
  static constexpr int kDefaultTapeRangeLookbackMinutes = 180;
  // Prints fetched by the admission probe when the tape-range check alone
  // needs samples.
  static constexpr int kDefaultTapeProbeTrades = 8;

  int min_price_cents{kDefaultMinPriceCents};
  int max_price_cents{kDefaultMaxPriceCents};
  int min_spread_cents{kDefaultMinSpreadCents};
  int max_spread_cents{kDefaultMaxSpreadCents};
  double min_volume_24h{kDefaultMinVolume24h};
  double min_days_to_close{kDefaultMinDaysToClose};
  double max_days_to_close{kDefaultMaxDaysToClose};
  double incentive_weight{kDefaultIncentiveWeight};
  int trade_top_n{kDefaultTradeTopN};
  int max_stale_trade_minutes{kDefaultMaxStaleTradeMinutes};
  int rotation_minutes{kDefaultRotationMinutes};
  int min_trades_per_hour{kDefaultMinTradesPerHour};
  int min_trade_price_range_cents{kDefaultMinTradePriceRangeCents};
  int tape_range_lookback_minutes{kDefaultTapeRangeLookbackMinutes};
  double min_minority_flow_ratio{kDefaultMinMinorityFlowRatio};
  double min_reversion_kappa{kDefaultMinReversionKappa};
  int reversion_window_minutes{kDefaultReversionWindowMinutes};
  // When non-empty, scan fetches each event series separately instead of
  // using the global /markets listing (which returns zero-volume junk).
  std::vector<std::string> event_series{};
};

} // namespace kalshi
