#!/usr/bin/env python3
"""Offline tape microstructure report over session logs (PLAN item 77).

Usage: python3 scripts/microstructure_report.py logs/session_frames.jsonl \
    logs/analytics.jsonl [more.jsonl ...] [--min-prints 100] [--bar-ms 5000]

Auto-detects record kinds across all given files: WS `trade` frames feed the
tape estimators, analytics `quote` records feed the quote-bar estimator;
everything else is ignored, so whole log directories can be globbed in.
Trades are deduplicated by trade_id across overlapping captures.

Tape estimators, per market (Krause Ch.5 — Kalshi's tape carries the exact
taker side, so every estimator is direct rather than inferred):
- First-order autocorrelation of trade-price changes, the regime
  discriminator: clearly negative = inventory/bounce regime (harvestable,
  safe to tighten); near zero or positive = informed flow (respect the
  spread floor).
- Roll effective-spread proxy 2*sqrt(-Cov[dp, dp_prev]), reported missing
  when the autocovariance is positive.
- Stoll gamma/delta spread decomposition. gamma = P(next trade flips taker
  side). With price changes signed against the previous trade's direction
  (x = -dir_prev * dp), E[x | repeat] = -delta*s and E[x | flip] =
  (1-delta)*s, so s = E[x|flip] - E[x|repeat] and delta falls out of the
  repeat mean. Component shares of the traded spread: adverse selection
  1 - 2*(gamma - delta), inventory 2*gamma - 1, order processing
  1 - 2*delta. Realized spread 2*(gamma - delta)*s is what a maker
  actually keeps at the next print.
Markets with fewer than --min-prints prints are pooled per series (ticker
prefix); differences and transitions are only ever formed within a single
market, never across markets.

Quote-bar estimator, per quoted ticker (Krause 2003 transitory-variance
split — needs only the mid series, no fills): mid is resampled to --bar-ms
bars; alpha = -Cov[dP, dP_prev] estimates the transitory (pricing-error)
variance, sigma2 = Var[dP] - 2*alpha the fundamental variance; transitory
share = 2*alpha / Var[dP]. Overidentification check: Cov[dP^2, dP_prev^2]
should be near 2*alpha^2 when the model fits.
"""

import argparse
import json
import math
from collections import defaultdict

MIN_PRINTS_DEFAULT = 100
BAR_MS_DEFAULT = 5_000
MIN_TRANSITIONS = 10
MIN_DIFF_PAIRS = 10
MIN_BARS = 30
BOUNCE_AUTOCORR_MAX = -0.15
INFORMED_AUTOCORR_MIN = -0.05
OVERID_OK_LOW = 0.5
OVERID_OK_HIGH = 2.0
OVERID_MIN_TRANSITORY_SHARE = 0.05
CENTS_PER_DOLLAR = 100.0


def load_records(paths):
    trades_by_market, quotes_by_ticker = defaultdict(dict), defaultdict(list)
    for path in paths:
        with open(path) as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    continue
                kind = record.get("type")
                if kind == "trade" and "msg" in record:
                    message = record["msg"]
                    price_cents = float(message["yes_price_dollars"]) * CENTS_PER_DOLLAR
                    direction = 1 if message["taker_side"] == "yes" else -1
                    trades_by_market[message["market_ticker"]][message["trade_id"]] = (
                        message["ts_ms"],
                        price_cents,
                        direction,
                    )
                elif kind == "quote" and "ticker" in record:
                    quotes_by_ticker[record["ticker"]].append(
                        (record["ts_ms"], record["mid"])
                    )
    sorted_trades = {
        ticker: sorted(prints.values())
        for ticker, prints in trades_by_market.items()
    }
    for series in quotes_by_ticker.values():
        series.sort()
    return sorted_trades, quotes_by_ticker


def mean(values):
    return sum(values) / len(values) if values else None


def covariance(pairs):
    if len(pairs) < 2:
        return None
    first_mean = mean([pair[0] for pair in pairs])
    second_mean = mean([pair[1] for pair in pairs])
    return mean([(a - first_mean) * (b - second_mean) for a, b in pairs])


def correlation(pairs):
    cov = covariance(pairs)
    if cov is None:
        return None
    first_var = covariance([(a, a) for a, _ in pairs])
    second_var = covariance([(b, b) for _, b in pairs])
    if not first_var or not second_var:
        return None
    return cov / math.sqrt(first_var * second_var)


def market_observations(prints):
    diffs, diff_pairs, transitions = [], [], []
    for index in range(1, len(prints)):
        _, price, direction = prints[index]
        _, prev_price, prev_direction = prints[index - 1]
        price_change = price - prev_price
        diffs.append(price_change)
        transitions.append(
            (direction != prev_direction, -prev_direction * price_change)
        )
    for index in range(1, len(diffs)):
        diff_pairs.append((diffs[index - 1], diffs[index]))
    return diffs, diff_pairs, transitions


def regime_label(autocorr):
    if autocorr is None:
        return "n/a"
    if autocorr <= BOUNCE_AUTOCORR_MAX:
        return "bounce"
    if autocorr >= INFORMED_AUTOCORR_MIN:
        return "informed"
    return "mixed"


def tape_stats(per_market_prints):
    diff_pairs, transitions, print_count = [], [], 0
    for prints in per_market_prints:
        print_count += len(prints)
        _, market_diff_pairs, market_transitions = market_observations(prints)
        diff_pairs.extend(market_diff_pairs)
        transitions.extend(market_transitions)
    stats = {"prints": print_count, "markets": len(per_market_prints)}
    autocorr = (
        correlation(diff_pairs) if len(diff_pairs) >= MIN_DIFF_PAIRS else None
    )
    stats["autocorr"] = autocorr
    stats["regime"] = regime_label(autocorr)
    autocov = covariance(diff_pairs) if len(diff_pairs) >= MIN_DIFF_PAIRS else None
    stats["roll"] = (
        2.0 * math.sqrt(-autocov) if autocov is not None and autocov < 0 else None
    )
    stats.update(
        {"gamma": None, "delta": None, "spread": None, "shares": None,
         "realized": None}
    )
    if len(transitions) < MIN_TRANSITIONS:
        return stats
    flips = [signed for flipped, signed in transitions if flipped]
    repeats = [signed for flipped, signed in transitions if not flipped]
    stats["gamma"] = len(flips) / len(transitions)
    if not flips or not repeats:
        return stats
    spread = mean(flips) - mean(repeats)
    if spread <= 0:
        return stats
    delta = -mean(repeats) / spread
    gamma = stats["gamma"]
    stats["delta"] = delta
    stats["spread"] = spread
    stats["shares"] = (
        1.0 - 2.0 * (gamma - delta),
        2.0 * gamma - 1.0,
        1.0 - 2.0 * delta,
    )
    stats["realized"] = 2.0 * (gamma - delta) * spread
    return stats


def resample_to_bars(series, bar_ms):
    bars, bar_index, last_mid = [], 0, None
    first_ts = series[0][0]
    for ts_ms, mid in series:
        while first_ts + (bar_index + 1) * bar_ms <= ts_ms:
            if last_mid is not None:
                bars.append(last_mid)
            bar_index += 1
        last_mid = mid
    bars.append(last_mid)
    return bars


def bar_stats(series, bar_ms):
    bars = resample_to_bars(series, bar_ms)
    if len(bars) < MIN_BARS:
        return None
    diffs = [bars[index] - bars[index - 1] for index in range(1, len(bars))]
    diff_pairs = [(diffs[index - 1], diffs[index]) for index in range(1, len(diffs))]
    autocov = covariance(diff_pairs)
    variance = covariance([(diff, diff) for diff in diffs])
    if autocov is None or not variance:
        return None
    alpha = -autocov
    stats = {
        "bars": len(bars),
        "alpha": alpha,
        "fundamental_var": variance - 2.0 * alpha,
        "transitory_share": 2.0 * alpha / variance if alpha > 0 else 0.0,
        "overid_ratio": None,
    }
    squared_cov = covariance([(a * a, b * b) for a, b in diff_pairs])
    if (
        stats["transitory_share"] >= OVERID_MIN_TRANSITORY_SHARE
        and squared_cov is not None
    ):
        stats["overid_ratio"] = squared_cov / (2.0 * alpha * alpha)
    return stats


def fmt(value, precision=2):
    return "-" if value is None else f"{value:.{precision}f}"


def fmt_pct(value):
    return "-" if value is None else f"{100.0 * value:.0f}"


def print_tape_table(rows):
    header = (
        f"{'market':<42} {'prints':>6} {'acorr':>6} {'regime':>8} "
        f"{'roll_c':>6} {'s_c':>6} {'gamma':>5} {'delta':>5} "
        f"{'AS%':>4} {'INV%':>4} {'OP%':>4} {'rlzd_c':>6}"
    )
    print(header)
    print("-" * len(header))
    for label, stats in rows:
        shares = stats["shares"] or (None, None, None)
        print(
            f"{label:<42} {stats['prints']:>6} {fmt(stats['autocorr']):>6} "
            f"{stats['regime']:>8} {fmt(stats['roll']):>6} "
            f"{fmt(stats['spread']):>6} {fmt(stats['gamma']):>5} "
            f"{fmt(stats['delta']):>5} {fmt_pct(shares[0]):>4} "
            f"{fmt_pct(shares[1]):>4} {fmt_pct(shares[2]):>4} "
            f"{fmt(stats['realized']):>6}"
        )


def print_bar_table(rows, bar_ms):
    header = (
        f"{'ticker':<42} {'bars':>6} {'alpha':>7} {'fund_var':>8} "
        f"{'trans%':>6} {'overid':>7}"
    )
    print(f"\n=== Quote-bar transitory split (bar={bar_ms}ms) ===")
    print(header)
    print("-" * len(header))
    for ticker, stats in rows:
        ratio = stats["overid_ratio"]
        if stats["fundamental_var"] < 0:
            verdict = "violated"
        elif ratio is None:
            verdict = ""
        else:
            verdict = (
                "ok" if OVERID_OK_LOW <= ratio <= OVERID_OK_HIGH else "strained"
            )
        print(
            f"{ticker:<42} {stats['bars']:>6} {fmt(stats['alpha'], 3):>7} "
            f"{fmt(stats['fundamental_var'], 3):>8} "
            f"{fmt_pct(stats['transitory_share']):>6} "
            f"{fmt(ratio):>5} {verdict}"
        )


def series_prefix(ticker):
    return ticker.split("-")[0]


def build_tape_rows(trades_by_market, min_prints):
    rows, pooled = [], defaultdict(list)
    for ticker, prints in trades_by_market.items():
        if len(prints) >= min_prints:
            rows.append((ticker, tape_stats([prints])))
        else:
            pooled[series_prefix(ticker)].append(prints)
    for series, market_prints in pooled.items():
        stats = tape_stats(market_prints)
        label = f"{series} (pooled, {stats['markets']} mkts)"
        rows.append((label, stats))
    rows.sort(key=lambda row: row[1]["prints"], reverse=True)
    return rows


def main():
    parser = argparse.ArgumentParser(
        description="Tape microstructure report (PLAN item 77)"
    )
    parser.add_argument("files", nargs="+")
    parser.add_argument("--min-prints", type=int, default=MIN_PRINTS_DEFAULT)
    parser.add_argument("--bar-ms", type=int, default=BAR_MS_DEFAULT)
    arguments = parser.parse_args()

    trades_by_market, quotes_by_ticker = load_records(arguments.files)
    total_prints = sum(len(prints) for prints in trades_by_market.values())
    print(
        f"=== Tape microstructure ({len(arguments.files)} files, "
        f"{total_prints} prints, {len(trades_by_market)} markets) ==="
    )
    if trades_by_market:
        print_tape_table(build_tape_rows(trades_by_market, arguments.min_prints))
    else:
        print("no trade frames found")

    bar_rows = []
    for ticker, series in sorted(quotes_by_ticker.items()):
        stats = bar_stats(series, arguments.bar_ms)
        if stats is not None:
            bar_rows.append((ticker, stats))
    if bar_rows:
        print_bar_table(bar_rows, arguments.bar_ms)


if __name__ == "__main__":
    main()
