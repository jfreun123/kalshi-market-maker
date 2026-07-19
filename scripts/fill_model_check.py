#!/usr/bin/env python3
"""Validate the backtest fill model against captured tape (PLAN item 72).

Usage: python3 scripts/fill_model_check.py logs/session_frames.jsonl \
    [more session_frames.jsonl ...] [--window-ms 50]

The paper/backtest fill model is deliberately conservative: a public print
fills a resting order only when the taker traded STRICTLY through its level
(at-level prints are queue-position-dependent and never simulated). This
script measures what that conservatism ignores, ND-HFTT style: every trade
print is classified against the passive side's best level just before it
(at-BBO vs through vs behind), and matched to negative orderbook deltas at
the printed level within ±--window-ms (book consumption confirming where
the print actually ate).

Output per market and in aggregate: print counts and volume by class, the
share of volume the strict-through model can never fill (at-BBO prints),
and the delta-match rate (prints whose consumption is visible in the book —
a sanity check that the tape and book streams line up). If the at-BBO share
is large, the backtest under-fills and a proportional delta-consumption
model is warranted before trusting backtest verdicts (e.g. the
clearing-pricing A/B).

Trades are deduplicated by trade_id across overlapping captures.
"""

import argparse
import json
from bisect import bisect_left, bisect_right
from collections import defaultdict

WINDOW_MS_DEFAULT = 50
CENTS_PER_DOLLAR = 100.0
PRICE_BASIS_CENTS = 100.0


def load_frames(paths):
    events_by_market = defaultdict(list)
    seen_trades = set()
    for path in paths:
        books = {}
        for line in open(path):
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            kind = record.get("type")
            if "msg" not in record:
                continue
            message = record["msg"]
            if kind == "orderbook_snapshot":
                ticker = message["market_ticker"]
                book = {"yes": {}, "no": {}}
                for side in ("yes", "no"):
                    for price_str, qty_str in message.get(
                        f"{side}_dollars_fp", []
                    ):
                        book[side][float(price_str) * CENTS_PER_DOLLAR] = float(
                            qty_str
                        )
                books[ticker] = book
            elif kind == "orderbook_delta":
                ticker = message["market_ticker"]
                if ticker not in books:
                    continue
                side = message["side"]
                price = float(message["price_dollars"]) * CENTS_PER_DOLLAR
                change = float(message["delta_fp"])
                book = books[ticker]
                quantity = book[side].get(price, 0.0) + change
                if quantity > 0:
                    book[side][price] = quantity
                else:
                    book[side].pop(price, None)
                events_by_market[ticker].append(
                    ("delta", message["ts_ms"], side, price, change)
                )
            elif kind == "trade":
                trade_id = message["trade_id"]
                if trade_id in seen_trades:
                    continue
                seen_trades.add(trade_id)
                ticker = message["market_ticker"]
                best = {}
                book = books.get(ticker)
                if book is not None:
                    best = {
                        side: max(book[side]) if book[side] else None
                        for side in ("yes", "no")
                    }
                events_by_market[ticker].append(
                    (
                        "trade",
                        message["ts_ms"],
                        message["taker_side"],
                        float(message["yes_price_dollars"]) * CENTS_PER_DOLLAR,
                        float(message["count_fp"]),
                        best,
                    )
                )
    return events_by_market


def classify_print(taker_side, yes_price_cents, best):
    if taker_side == "yes":
        passive_best_no = best.get("no")
        if passive_best_no is None:
            return "no_book", None, None
        best_ask_cents = PRICE_BASIS_CENTS - passive_best_no
        passive_level = PRICE_BASIS_CENTS - yes_price_cents
        if yes_price_cents > best_ask_cents:
            return "through", "no", passive_level
        if yes_price_cents == best_ask_cents:
            return "at_bbo", "no", passive_level
        return "behind", "no", passive_level
    passive_best_yes = best.get("yes")
    if passive_best_yes is None:
        return "no_book", None, None
    if yes_price_cents < passive_best_yes:
        return "through", "yes", yes_price_cents
    if yes_price_cents == passive_best_yes:
        return "at_bbo", "yes", yes_price_cents
    return "behind", "yes", yes_price_cents


def analyze_market(events, window_ms):
    deltas = [event for event in events if event[0] == "delta"]
    delta_times = [event[1] for event in deltas]
    stats = defaultdict(float)
    counts = defaultdict(int)
    matched_prints = 0
    total_prints = 0
    for event in events:
        if event[0] != "trade":
            continue
        _, ts_ms, taker_side, yes_price, volume, best = event
        total_prints += 1
        label, passive_side, passive_level = classify_print(
            taker_side, yes_price, best
        )
        counts[label] += 1
        stats[label] += volume
        if passive_side is None:
            continue
        low = bisect_left(delta_times, ts_ms - window_ms)
        high = bisect_right(delta_times, ts_ms + window_ms)
        consumed = sum(
            -change
            for _, _, side, price, change in deltas[low:high]
            if side == passive_side and price == passive_level and change < 0
        )
        if consumed > 0:
            matched_prints += 1
    return stats, counts, matched_prints, total_prints


def main():
    parser = argparse.ArgumentParser(
        description="Backtest fill-model validation (PLAN item 72)"
    )
    parser.add_argument("files", nargs="+")
    parser.add_argument("--window-ms", type=int, default=WINDOW_MS_DEFAULT)
    arguments = parser.parse_args()

    events_by_market = load_frames(arguments.files)
    header = (
        f"{'market':<42} {'prints':>6} {'thru%':>6} {'atbbo%':>6} "
        f"{'behind%':>7} {'nobk%':>5} {'match%':>6}"
    )
    print(
        f"=== Fill-model check (±{arguments.window_ms}ms delta window; "
        f"volume shares) ==="
    )
    print(header)
    print("-" * len(header))
    total = defaultdict(float)
    total_prints = 0
    total_matched = 0
    for ticker in sorted(events_by_market):
        stats, counts, matched, prints = analyze_market(
            events_by_market[ticker], arguments.window_ms
        )
        if prints == 0:
            continue
        volume = sum(stats.values())
        if volume == 0:
            continue
        for label, amount in stats.items():
            total[label] += amount
        total_prints += prints
        total_matched += matched
        print(
            f"{ticker:<42} {prints:>6} "
            f"{100.0 * stats['through'] / volume:>6.1f} "
            f"{100.0 * stats['at_bbo'] / volume:>6.1f} "
            f"{100.0 * stats['behind'] / volume:>7.1f} "
            f"{100.0 * stats['no_book'] / volume:>5.1f} "
            f"{100.0 * matched / prints:>6.1f}"
        )
    volume = sum(total.values())
    if volume == 0:
        print("no prints found")
        return
    at_share = 100.0 * total["at_bbo"] / volume
    print(
        f"\nTOTAL {total_prints} prints: through {100.0 * total['through'] / volume:.1f}% · "
        f"at-BBO {at_share:.1f}% · behind {100.0 * total['behind'] / volume:.1f}% · "
        f"no-book {100.0 * total['no_book'] / volume:.1f}% · "
        f"delta-match {100.0 * total_matched / total_prints:.1f}%"
    )
    print(
        f"strict print-through backtest model ignores ~{at_share:.1f}% of "
        f"print volume (at-BBO prints)"
    )


if __name__ == "__main__":
    main()
