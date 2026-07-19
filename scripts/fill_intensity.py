#!/usr/bin/env python3
"""Empirical fill-intensity curve lambda(delta) from analytics JSONL
(PLAN item 79).

Usage: python3 scripts/fill_intensity.py logs/analytics.jsonl [more.jsonl ...]
    [--gap-cap-ms 30000] [--max-delta 10] [--per-ticker]

For every quote event the resting distance of each side from our fair value
(delta_bid = fv - bid, delta_ask = ask - fv, in cents) is exposed for the
duration until the next quote event on that ticker (capped at --gap-cap-ms
so rotation gaps don't count as exposure). Maker fills are assigned the
distance between their price and the last fv at or before the fill, on the
filled side. Fill counts over exposure hours per whole-cent distance bucket
give the empirical intensity curve lambda(delta) in fills/hour — the input
every optimal-quoting model in Krause Ch.3 shares (Ho-Stoll spreads, the
monopoly term, FKK layer rungs, principled rest timers).

Two exposure-weighted least-squares fits over buckets within --max-delta:
- Exponential lambda = A * exp(-k * delta) (Avellaneda-Stoikov form), fitted
  on log intensity over buckets with at least one fill.
- Linear lambda = alpha - beta * delta; when beta > 0 the implied
  sole-quoter (monopoly) half-spread alpha / (2 * beta) is reported
  (Ho-Stoll 1981) — only meaningful in books where we are effectively the
  only quoter.

Distances at or through fair land in bucket 0; distances beyond --max-delta
pool into an overflow bucket excluded from the fits.
"""

import argparse
import json
import math
from bisect import bisect_right
from collections import defaultdict

GAP_CAP_MS_DEFAULT = 30_000
MAX_DELTA_DEFAULT = 10
MS_PER_HOUR = 3_600_000.0
MIN_BUCKETS_FOR_FIT = 3


def load_events(paths):
    quotes_by_ticker, fills = defaultdict(list), []
    for path in paths:
        with open(path) as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                event = json.loads(line)
                if event.get("type") == "quote" and "ticker" in event:
                    quotes_by_ticker[event["ticker"]].append(
                        (event["ts_ms"], event["bid"], event["ask"], event["fv"])
                    )
                elif event.get("type") == "fill" and not event.get("is_taker"):
                    fills.append(event)
    for series in quotes_by_ticker.values():
        series.sort()
    return quotes_by_ticker, fills


def bucket_of(delta, max_delta):
    rounded = round(delta)
    if rounded < 0:
        return 0
    if rounded > max_delta:
        return max_delta + 1
    return int(rounded)


def accumulate_exposure(quotes_by_ticker, gap_cap_ms, max_delta):
    exposure_ms = {
        "bid": defaultdict(float),
        "ask": defaultdict(float),
    }
    for series in quotes_by_ticker.values():
        for index in range(len(series) - 1):
            ts_ms, bid, ask, fair = series[index]
            duration = min(series[index + 1][0] - ts_ms, gap_cap_ms)
            if duration <= 0:
                continue
            exposure_ms["bid"][bucket_of(fair - bid, max_delta)] += duration
            exposure_ms["ask"][bucket_of(ask - fair, max_delta)] += duration
    return exposure_ms


def fair_at(series, ts_ms):
    index = bisect_right(series, (ts_ms, float("inf"), 0, 0)) - 1
    if index < 0:
        return None
    return series[index][3]


def assign_fills(quotes_by_ticker, fills, max_delta):
    fill_counts = {"bid": defaultdict(int), "ask": defaultdict(int)}
    unassigned = 0
    for fill in fills:
        series = quotes_by_ticker.get(fill["ticker"], [])
        fair = fair_at(series, fill["ts_ms"])
        if fair is None:
            unassigned += 1
            continue
        if fill["side"] == "yes":
            quote_side = "bid"
            delta = fair - fill["price"]
        else:
            quote_side = "ask"
            delta = (100.0 - fill["price"]) - fair
        fill_counts[quote_side][bucket_of(delta, max_delta)] += 1
    return fill_counts, unassigned


def weighted_least_squares(points):
    total = sum(weight for _, _, weight in points)
    if total == 0:
        return None
    mean_x = sum(x * weight for x, _, weight in points) / total
    mean_y = sum(y * weight for _, y, weight in points) / total
    variance_x = sum(
        weight * (x - mean_x) ** 2 for x, _, weight in points
    ) / total
    if variance_x == 0:
        return None
    slope = sum(
        weight * (x - mean_x) * (y - mean_y) for x, y, weight in points
    ) / (total * variance_x)
    return mean_y - slope * mean_x, slope


def fit_exponential(rows):
    points = [
        (bucket, math.log(intensity), hours)
        for bucket, hours, fill_count, intensity in rows
        if fill_count > 0
    ]
    if len(points) < MIN_BUCKETS_FOR_FIT:
        return None
    fitted = weighted_least_squares(points)
    if fitted is None:
        return None
    intercept, slope = fitted
    return math.exp(intercept), -slope


def fit_linear(rows):
    points = [
        (bucket, intensity, hours)
        for bucket, hours, fill_count, intensity in rows
        if hours > 0
    ]
    if len(points) < MIN_BUCKETS_FOR_FIT:
        return None
    fitted = weighted_least_squares(points)
    if fitted is None:
        return None
    intercept, slope = fitted
    return intercept, -slope


def bucket_rows(exposure_ms, fill_counts, max_delta):
    rows = []
    for bucket in range(max_delta + 2):
        hours = (
            exposure_ms["bid"][bucket] + exposure_ms["ask"][bucket]
        ) / MS_PER_HOUR
        fill_count = fill_counts["bid"][bucket] + fill_counts["ask"][bucket]
        intensity = fill_count / hours if hours > 0 else None
        rows.append((bucket, hours, fill_count, intensity))
    return rows


def bucket_label(bucket, max_delta):
    if bucket == 0:
        return "<=0"
    if bucket > max_delta:
        return f">{max_delta}"
    return str(bucket)


def print_report(rows, max_delta, label):
    print(f"\n=== lambda(delta) — {label} ===")
    print(f"{'delta_c':>7} {'hours':>8} {'fills':>6} {'fills/hr':>9}")
    for bucket, hours, fill_count, intensity in rows:
        if hours == 0 and fill_count == 0:
            continue
        shown = f"{intensity:.3f}" if intensity is not None else "-"
        print(
            f"{bucket_label(bucket, max_delta):>7} {hours:>8.2f} "
            f"{fill_count:>6} {shown:>9}"
        )
    fit_rows = [row for row in rows if row[0] <= max_delta]
    exponential = fit_exponential(fit_rows)
    if exponential is not None:
        amplitude, decay = exponential
        print(
            f"exponential fit: lambda = {amplitude:.2f} * exp(-{decay:.3f} * "
            f"delta) fills/hr"
        )
    else:
        print("exponential fit: insufficient buckets with fills")
    linear = fit_linear(fit_rows)
    if linear is not None:
        alpha, beta = linear
        print(f"linear fit: lambda = {alpha:.2f} - {beta:.3f} * delta")
        if beta > 0:
            print(
                f"implied sole-quoter half-spread alpha/(2*beta) = "
                f"{alpha / (2.0 * beta):.1f}c"
            )
    else:
        print("linear fit: insufficient buckets")


def main():
    parser = argparse.ArgumentParser(
        description="Empirical fill-intensity curve (PLAN item 79)"
    )
    parser.add_argument("files", nargs="+")
    parser.add_argument("--gap-cap-ms", type=int, default=GAP_CAP_MS_DEFAULT)
    parser.add_argument("--max-delta", type=int, default=MAX_DELTA_DEFAULT)
    parser.add_argument("--per-ticker", action="store_true")
    arguments = parser.parse_args()

    quotes_by_ticker, fills = load_events(arguments.files)
    exposure_ms = accumulate_exposure(
        quotes_by_ticker, arguments.gap_cap_ms, arguments.max_delta
    )
    fill_counts, unassigned = assign_fills(
        quotes_by_ticker, fills, arguments.max_delta
    )
    total_hours = sum(
        sum(side.values()) for side in exposure_ms.values()
    ) / MS_PER_HOUR
    print(
        f"{len(fills)} maker fills ({unassigned} without a prior quote), "
        f"{total_hours:.1f} side-hours of quote exposure, "
        f"{len(quotes_by_ticker)} tickers"
    )
    print_report(
        bucket_rows(exposure_ms, fill_counts, arguments.max_delta),
        arguments.max_delta,
        "all tickers, both sides",
    )

    if arguments.per_ticker:
        for ticker in sorted(quotes_by_ticker):
            ticker_quotes = {ticker: quotes_by_ticker[ticker]}
            ticker_fills = [
                fill for fill in fills if fill["ticker"] == ticker
            ]
            ticker_exposure = accumulate_exposure(
                ticker_quotes, arguments.gap_cap_ms, arguments.max_delta
            )
            ticker_counts, _ = assign_fills(
                ticker_quotes, ticker_fills, arguments.max_delta
            )
            print_report(
                bucket_rows(ticker_exposure, ticker_counts, arguments.max_delta),
                arguments.max_delta,
                ticker,
            )


if __name__ == "__main__":
    main()
