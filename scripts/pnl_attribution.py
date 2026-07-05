#!/usr/bin/env python3
"""Why did we make/lose money? PnL attribution over analytics JSONL.

Usage: python3 scripts/pnl_attribution.py logs/analytics_*.jsonl

Decomposes the session into named causes, per ticker and total:
  entry_edge     — half-spread earned (or given) at each maker fill vs the
                   mid at fill time; positive = we were paid to provide
  drift          — mid movement while inventory was held (adverse selection
                   / directional exposure); the "market moved against us" term
  exit_cost      — spread paid when a position was closed by a taker order
                   (e.g. the session-end flatten)
  picked_off     — count of fills where the belief (fv) had already moved
                   against the quote in the 5s before the fill: a latency loss
Per-fill table shows edge, quote age at fill, and pre-fill belief drift so
"latency vs bad pricing" is answerable per event, not by vibes.
"""

import json
import sys
from bisect import bisect_right
from collections import defaultdict

PICKOFF_WINDOW_MS = 5_000
PICKOFF_DRIFT_CENTS = 1.0


def native(price_yes, side):
    return price_yes if side == "yes" else 100.0 - price_yes


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    quotes = defaultdict(list)
    fills = []
    for path in sys.argv[1:]:
        for line in open(path):
            line = line.strip()
            if not line:
                continue
            ev = json.loads(line)
            if ev["type"] == "quote":
                quotes[ev["ticker"]].append((ev["ts_ms"], ev["mid"], ev["fv"]))
            elif ev["type"] == "fill":
                fills.append(ev)
    for s in quotes.values():
        s.sort()
    fills.sort(key=lambda e: e["ts_ms"])
    if not fills:
        print("no fills — nothing to attribute")
        return 0

    def at(ticker, ts):
        s = quotes.get(ticker, [])
        i = bisect_right(s, (ts, float("inf"), float("inf"))) - 1
        return s[i] if i >= 0 else None

    print(f"{'ts_ms':>14} {'side':<4} {'price':>5} {'qty':>6} {'taker':<5} "
          f"{'edge_c':>7} {'predrift':>8} note")
    totals = defaultdict(float)
    pickoffs = 0
    inventory = defaultdict(float)   # signed YES contracts
    entry_mid = {}

    for f in fills:
        tick, ts = f["ticker"], f["ts_ms"]
        now_q = at(tick, ts)
        mid = f.get("mid") or (now_q[1] if now_q else None)
        qty = f["qty"]
        signed = qty if f["side"] == "yes" else -qty
        edge = None
        if mid:
            edge = native(mid, f["side"]) - f["price"] if f["is_taker"] else \
                   native(mid, f["side"]) - f["price"]
            # maker: we BUY side at price; positive edge = bought below value
            edge = native(mid, f["side"]) - f["price"]
        before = at(tick, ts - PICKOFF_WINDOW_MS)
        predrift = None
        if before and now_q:
            predrift = native(now_q[2], f["side"]) - native(before[2], f["side"])
        note = ""
        if predrift is not None and predrift < -PICKOFF_DRIFT_CENTS:
            pickoffs += 1
            note = "PICKED-OFF (belief fell before fill)"
        bucket = "exit_cost" if f["is_taker"] else "entry_edge"
        if edge is not None:
            totals[bucket] += edge * qty / 100.0
        # drift while holding: settle previous inventory at this mid
        if mid is not None and inventory[tick] != 0 and tick in entry_mid:
            totals["drift"] += inventory[tick] * (mid - entry_mid[tick]) / 100.0
        if mid is not None:
            entry_mid[tick] = mid
        inventory[tick] += signed
        print(f"{ts:>14} {f['side']:<4} {f['price']:>5} {qty:>6.2f} "
              f"{str(f['is_taker']):<5} "
              f"{edge:>+7.2f} " if edge is not None else f"{'n/a':>7} ",
              end="")
        print(f"{predrift:>+8.2f} {note}" if predrift is not None
              else f"{'n/a':>8} {note}")

    total = sum(totals.values())
    print("\n== attribution (dollars) ==")
    for k in ["entry_edge", "drift", "exit_cost"]:
        print(f"  {k:<12} {totals[k]:>+8.3f}")
    print(f"  {'total':<12} {total:>+8.3f}")
    print(f"  picked-off fills: {pickoffs}/{len(fills)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
