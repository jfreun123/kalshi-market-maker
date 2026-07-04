#!/usr/bin/env python3
"""Offline fill-quality analysis over analytics JSONL files (PLAN item 31).

Usage: python3 scripts/analyze_fills.py logs/analytics.jsonl [more.jsonl ...]

For every fill event, joins the mid-price series from quote events on the same
ticker to compute markout (mark at +30s and +5min, side-native cents per
contract: positive = the market moved our way after the fill) and effective
spread (2 x |price - mid| at fill). Prints a per-fill table and per-ticker /
overall summaries. Markout is the Gate 1 evaluation metric (see
docs/PRE_LIVE_CHECKLIST.md) and must never feed back into quoting.
"""

import json
import sys
from bisect import bisect_right
from collections import defaultdict

MARKOUT_HORIZONS_MS = {"30s": 30_000, "5min": 300_000}


def load_events(paths):
    quotes, fills = defaultdict(list), []
    for path in paths:
        with open(path) as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                event = json.loads(line)
                if event["type"] == "quote":
                    quotes[event["ticker"]].append((event["ts_ms"], event["mid"]))
                elif event["type"] == "fill":
                    fills.append(event)
    for series in quotes.values():
        series.sort()
    fills.sort(key=lambda event: event["ts_ms"])
    return quotes, fills


def mid_at(series, ts_ms):
    """Last observed mid at or before ts_ms; None if the series ends earlier."""
    if not series:
        return None
    index = bisect_right(series, (ts_ms, float("inf"))) - 1
    if index < 0:
        return None
    if series[-1][0] < ts_ms:
        return None  # series ended before the horizon — no honest mark
    return series[index][1]


def side_native(mid, side):
    return mid if side == "yes" else 100.0 - mid


def analyze(quotes, fills):
    rows = []
    for fill in fills:
        series = quotes.get(fill["ticker"], [])
        mid_now = fill.get("mid") or mid_at(series, fill["ts_ms"])
        row = {
            "ts_ms": fill["ts_ms"],
            "ticker": fill["ticker"],
            "side": fill["side"],
            "price": fill["price"],
            "qty": fill["qty"],
            "is_taker": fill["is_taker"],
            "eff_spread": None,
            "markout": {},
        }
        if mid_now:
            row["eff_spread"] = 2.0 * abs(
                fill["price"] - side_native(mid_now, fill["side"])
            )
        for label, horizon in MARKOUT_HORIZONS_MS.items():
            mid_later = mid_at(series, fill["ts_ms"] + horizon)
            if mid_later is not None:
                row["markout"][label] = (
                    side_native(mid_later, fill["side"]) - fill["price"]
                )
        rows.append(row)
    return rows


def weighted_mean(pairs):
    total_qty = sum(qty for _, qty in pairs)
    if total_qty == 0:
        return None
    return sum(value * qty for value, qty in pairs) / total_qty


def summarize(rows, label):
    print(f"\n== {label} ({len(rows)} fills) ==")
    for horizon in MARKOUT_HORIZONS_MS:
        pairs = [
            (row["markout"][horizon], row["qty"])
            for row in rows
            if horizon in row["markout"]
        ]
        mean = weighted_mean(pairs)
        shown = f"{mean:+.2f}c/contract over {len(pairs)} fills" if pairs else "n/a"
        print(f"  markout@{horizon}: {shown}")
    spreads = [(row["eff_spread"], row["qty"]) for row in rows if row["eff_spread"]]
    mean_spread = weighted_mean(spreads)
    print(f"  eff_spread: {mean_spread:.2f}c" if spreads else "  eff_spread: n/a")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    quotes, fills = load_events(sys.argv[1:])
    rows = analyze(quotes, fills)
    if not rows:
        print("no fill events found")
        return 0

    print(f"{'ts_ms':>14} {'ticker':<34} {'side':<4} {'price':>5} {'qty':>7} "
          f"{'taker':<5} {'m@30s':>7} {'m@5min':>7} {'espr':>6}")
    for row in rows:
        m30 = row["markout"].get("30s")
        m5 = row["markout"].get("5min")
        print(f"{row['ts_ms']:>14} {row['ticker']:<34} {row['side']:<4} "
              f"{row['price']:>5} {row['qty']:>7.2f} {str(row['is_taker']):<5} "
              f"{m30:>+7.2f}" if m30 is not None else
              f"{row['ts_ms']:>14} {row['ticker']:<34} {row['side']:<4} "
              f"{row['price']:>5} {row['qty']:>7.2f} {str(row['is_taker']):<5} "
              f"{'n/a':>7}", end="")
        print(f" {m5:>+7.2f}" if m5 is not None else f" {'n/a':>7}", end="")
        espr = row["eff_spread"]
        print(f" {espr:>6.2f}" if espr is not None else f" {'n/a':>6}")

    by_ticker = defaultdict(list)
    for row in rows:
        by_ticker[row["ticker"]].append(row)
    for ticker, ticker_rows in sorted(by_ticker.items()):
        summarize(ticker_rows, ticker)
    maker_rows = [row for row in rows if not row["is_taker"]]
    if maker_rows:
        summarize(maker_rows, "ALL MAKER FILLS")
    summarize(rows, "ALL FILLS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
