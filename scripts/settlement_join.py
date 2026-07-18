#!/usr/bin/env python3
"""Settlement join + Brier scoring over analytics JSONL (PLAN item 31a).

Usage:
  python3 scripts/settlement_join.py logs/analytics_*.jsonl
  python3 scripts/settlement_join.py --prod logs/analytics_*.jsonl
  python3 scripts/settlement_join.py --results logs/settlements.json logs/*.jsonl

Joins the session's quote and fill streams with each market's settled outcome
(`GET /markets/{ticker}` is public; the winning side pays 100c, the loser 0c):

  ground-truth PnL  — every fill revalued at settlement instead of the last
                      mark, minus fees: the session's true PnL with no
                      mark-to-market assumption. The delta vs the mark-based
                      figure is how much the session-end mid lied.
  carried inventory — net contracts held after the last fill and what
                      settlement paid them (~0 after a clean flatten;
                      nonzero after a crash).
  Brier scores      — time-weighted squared error of each probability stream
                      (our fv, the book mid, the micro price) against the
                      outcome. fv beating mid means our beliefs added
                      information beyond the book; fv losing to mid means
                      drift losses are a pricing problem, not a latency
                      problem — the Hosting-gate discriminator.
  calibration       — time-weighted forecast-vs-frequency buckets accumulated
                      across all input files; grows with every soak session.

Markets not yet finalized are listed as pending — rerun after settlement.
Settled outcomes are cached in the --results file so each is fetched once.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from bisect import bisect_right
from collections import defaultdict

DEMO_BASE = "https://demo-api.kalshi.co/trade-api/v2"
PROD_BASE = "https://api.elections.kalshi.com/trade-api/v2"
DEFAULT_RESULTS_PATH = "logs/settlement_results.json"
TERMINAL_RESULTS = ("yes", "no")
CALIBRATION_BUCKET_CENTS = 10
MS_PER_HOUR = 3_600_000.0


def fetch_market(base_url, ticker):
    request = urllib.request.Request(
        f"{base_url}/markets/{ticker}",
        headers={"User-Agent": "kalshi-mm-settlement-join"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)["market"]


def resolve_results(tickers, base_url, cache_path):
    cache = {}
    if cache_path and os.path.exists(cache_path):
        with open(cache_path) as handle:
            cache = json.load(handle)
    results, fetched = {}, False
    for ticker in sorted(tickers):
        cached = cache.get(ticker)
        if cached and cached.get("result") in TERMINAL_RESULTS:
            results[ticker] = cached
            continue
        try:
            market = fetch_market(base_url, ticker)
            results[ticker] = {
                "result": market.get("result", ""),
                "status": market.get("status", ""),
                "settlement_ts": market.get("settlement_ts", ""),
            }
        except (urllib.error.URLError, KeyError, json.JSONDecodeError) as error:
            results[ticker] = {"result": "", "status": f"fetch-error: {error}"}
            continue
        if results[ticker]["result"] in TERMINAL_RESULTS:
            cache[ticker] = results[ticker]
            fetched = True
    if cache_path and fetched:
        os.makedirs(os.path.dirname(cache_path) or ".", exist_ok=True)
        with open(cache_path, "w") as handle:
            json.dump(cache, handle, indent=1, sort_keys=True)
    return results


def load_events(paths):
    quotes, fills = defaultdict(list), defaultdict(list)
    for path in paths:
        with open(path) as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                event = json.loads(line)
                if event["type"] == "quote":
                    quotes[event["ticker"]].append(
                        (event["ts_ms"], event["mid"], event["fv"], event["micro"])
                    )
                elif event["type"] == "fill":
                    fills[event["ticker"]].append(event)
    for series in quotes.values():
        series.sort()
    for series in fills.values():
        series.sort(key=lambda event: event["ts_ms"])
    return quotes, fills


def side_native(price_yes_cents, side):
    return price_yes_cents if side == "yes" else 100.0 - price_yes_cents


def settle_native_cents(result, side):
    return 100.0 if result == side else 0.0


def fills_pnl_dollars(ticker_fills, value_cents_of_fill):
    total_cents = 0.0
    for fill in ticker_fills:
        value = value_cents_of_fill(fill)
        if value is None:
            return None
        total_cents += fill["qty"] * (value - fill["price"])
        total_cents -= fill.get("fee_cents", 0.0)
    return total_cents / 100.0


def last_mid_cents(ticker_quotes, ts_ms):
    index = bisect_right(
        ticker_quotes, (ts_ms, float("inf"), float("inf"), float("inf"))
    ) - 1
    return ticker_quotes[index][1] if index >= 0 else None


def carried_contracts(ticker_fills):
    return sum(
        fill["qty"] if fill["side"] == "yes" else -fill["qty"]
        for fill in ticker_fills
    )


def time_weighted_samples(ticker_quotes, stream_index):
    samples = []
    for current, following in zip(ticker_quotes, ticker_quotes[1:]):
        weight_ms = following[0] - current[0]
        if weight_ms > 0:
            samples.append((current[stream_index] / 100.0, weight_ms))
    return samples


def brier(samples, outcome):
    total_weight = sum(weight for _, weight in samples)
    if total_weight <= 0:
        return None
    error = sum(weight * (prob - outcome) ** 2 for prob, weight in samples)
    return error / total_weight


def print_market_table(tickers, results, quotes, fills):
    print(f"{'ticker':<42} {'result':<7} {'fills':>5} {'carried':>8} "
          f"{'mark_$':>8} {'truth_$':>8} {'delta_$':>8}")
    for ticker in tickers:
        result = results[ticker]["result"]
        ticker_fills = fills.get(ticker, [])
        carried = carried_contracts(ticker_fills)
        final_ts = ticker_fills[-1]["ts_ms"] if ticker_fills else 0
        closing_mid = last_mid_cents(quotes.get(ticker, []), final_ts or 2**62)
        mark = fills_pnl_dollars(
            ticker_fills,
            lambda fill: side_native(closing_mid, fill["side"])
            if closing_mid is not None else None,
        )
        truth = fills_pnl_dollars(
            ticker_fills,
            lambda fill: settle_native_cents(result, fill["side"]),
        )
        delta = truth - mark if mark is not None and truth is not None else None
        print(f"{ticker:<42} {result:<7} {len(ticker_fills):>5} {carried:>+8.1f} "
              f"{format_dollars(mark):>8} {format_dollars(truth):>8} "
              f"{format_dollars(delta):>8}")


def format_dollars(value):
    return f"{value:+.2f}" if value is not None else "n/a"


def format_brier(value):
    return f"{value:.4f}" if value is not None else "n/a"


STREAMS = (("fv", 2), ("mid", 1), ("micro", 3))


def print_brier_table(tickers, results, quotes):
    print("\n== Brier (time-weighted; lower is better) ==")
    print(f"{'ticker':<42} {'hours':>6} {'fv':>8} {'mid':>8} {'micro':>8} "
          f"{'fv_edge':>8}")
    pooled = {name: [] for name, _ in STREAMS}
    for ticker in tickers:
        outcome = 1.0 if results[ticker]["result"] == "yes" else 0.0
        ticker_quotes = quotes.get(ticker, [])
        scores = {}
        for name, stream_index in STREAMS:
            samples = time_weighted_samples(ticker_quotes, stream_index)
            scores[name] = brier(samples, outcome)
            pooled[name].extend(
                (prob, weight, outcome) for prob, weight in samples
            )
        if all(score is None for score in scores.values()):
            continue
        edge = (scores["mid"] - scores["fv"]
                if scores["mid"] is not None and scores["fv"] is not None
                else None)
        hours = sum(
            weight for _, weight in time_weighted_samples(ticker_quotes, 1)
        ) / MS_PER_HOUR
        print(f"{ticker:<42} {hours:>6.2f} {format_brier(scores['fv']):>8} "
              f"{format_brier(scores['mid']):>8} "
              f"{format_brier(scores['micro']):>8} "
              f"{format_brier(edge):>8}")
    aggregate = {name: pooled_brier(samples) for name, samples in pooled.items()}
    print(f"{'AGGREGATE':<42} {'':>6} {format_brier(aggregate['fv']):>8} "
          f"{format_brier(aggregate['mid']):>8} "
          f"{format_brier(aggregate['micro']):>8}")
    if aggregate["fv"] is not None and aggregate["mid"] is not None:
        if aggregate["fv"] < aggregate["mid"]:
            print("verdict: fv BEATS mid — beliefs add information; "
                  "residual losses point at latency/queue, not pricing")
        else:
            print("verdict: fv does NOT beat mid — pricing problem; "
                  "lower latency would not have fixed these losses")


def pooled_brier(samples):
    total_weight = sum(weight for _, weight, _ in samples)
    if total_weight <= 0:
        return None
    error = sum(
        weight * (prob - outcome) ** 2 for prob, weight, outcome in samples
    )
    return error / total_weight


def print_calibration(tickers, results, quotes):
    for name, stream_index in STREAMS[:2]:
        buckets = defaultdict(lambda: [0.0, 0.0, 0.0])
        for ticker in tickers:
            outcome = 1.0 if results[ticker]["result"] == "yes" else 0.0
            samples = time_weighted_samples(quotes.get(ticker, []), stream_index)
            for prob, weight in samples:
                bucket = min(
                    int(prob * 100.0 / CALIBRATION_BUCKET_CENTS),
                    100 // CALIBRATION_BUCKET_CENTS - 1,
                )
                entry = buckets[bucket]
                entry[0] += weight
                entry[1] += weight * prob
                entry[2] += weight * outcome
        if not buckets:
            continue
        print(f"\n== calibration: {name} (time-weighted) ==")
        print(f"{'bucket':<10} {'hours':>7} {'forecast':>9} {'realized':>9}")
        for bucket in sorted(buckets):
            weight, prob_sum, outcome_sum = buckets[bucket]
            low = bucket * CALIBRATION_BUCKET_CENTS
            print(f"{low:>3}-{low + CALIBRATION_BUCKET_CENTS:<3}c   "
                  f"{weight / MS_PER_HOUR:>7.2f} {prob_sum / weight:>9.3f} "
                  f"{outcome_sum / weight:>9.3f}")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("paths", nargs="+", help="analytics JSONL file(s)")
    parser.add_argument("--prod", action="store_true",
                        help="fetch from production instead of demo")
    parser.add_argument("--base-url", default=None,
                        help="override the API base URL")
    parser.add_argument("--results", default=DEFAULT_RESULTS_PATH,
                        help="settled-outcome cache file")
    arguments = parser.parse_args()
    base_url = arguments.base_url or (PROD_BASE if arguments.prod else DEMO_BASE)

    quotes, fills = load_events(arguments.paths)
    tickers = sorted(set(quotes) | set(fills))
    if not tickers:
        print("no quote or fill events found")
        return 0
    results = resolve_results(tickers, base_url, arguments.results)
    settled = [t for t in tickers if results[t]["result"] in TERMINAL_RESULTS]
    unsettled = [t for t in tickers if t not in settled]

    if settled:
        print("== settlement join ==")
        print_market_table(settled, results, quotes, fills)
        print_brier_table(settled, results, quotes)
        print_calibration(settled, results, quotes)
    if unsettled:
        print("\n== not yet settled (rerun after finalization) ==")
        for ticker in unsettled:
            status = results[ticker]["status"] or "unknown"
            print(f"  {ticker:<42} {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
