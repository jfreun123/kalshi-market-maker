#!/usr/bin/env python3
"""Candle-tier fair-value backtest on production data (docs/BETTER_PRICING.md
Phase 3, PLAN item 66).

Usage:
  python3 scripts/backtest_fv_candles.py                 # auto-discover markets
  python3 scripts/backtest_fv_candles.py TICKER [...]    # score specific markets
  python3 scripts/backtest_fv_candles.py --hours 12 --max-markets 50

Scores fair-value candidates on next-print prediction error at 1-minute
candle granularity, using Kalshi's public production API (no auth). For each
minute t, every candidate computes fv from data up to t only; if minute t+1
traded, the candidate is graded against t+1's mean trade price. Reported per
spread bucket: MAE (accuracy, cents) and bias (signed lean, cents — the
error that becomes drift dollars against held inventory).

Candidates:
  mid        — (yes_bid.close + yes_ask.close) / 2; the book anchor
  last       — most recent trade price; naive tape
  tape(h=H)  — volume-weighted, exp-decayed VWAP of candle means (half-life H min)
  blend(w,h) — w * tape + (1-w) * mid; the BETTER_PRICING §3c formula

Candle-tier limits (see BETTER_PRICING §5/§6): candles carry no depth, so
micro-price and clearing-price variants are out of reach here — the book
proxy is the plain mid; candle means include bid-ask bounce; liquid
production books are not thin demo books. This tier RANKS candidates; the
tick-scale replay on our own captures (with a simulated-drift score)
confirms before anything reaches live quoting.

First results (2026-07-09) and interpretation: BETTER_PRICING.md §"Phase 3
results".
"""

import argparse
import json
import sys
import time
import urllib.request

BASE = "https://api.elections.kalshi.com/trade-api/v2"
HALF_LIVES_MINUTES = [2.0, 5.0, 15.0]
BLEND_WEIGHTS = [0.1, 0.25, 0.5, 0.75]
BLEND_HALF_LIFE_MINUTES = 5.0
MIN_TAPE_WEIGHT = 1e-6
DISCOVERY_PAGE_LIMIT = 30
DISCOVERY_UPDATED_WITHIN_HOURS = 2
DISCOVERY_POOL_SIZE = 120
SPREAD_BUCKETS = [
    ("spread <= 1.5c", lambda s: s <= 1.5),
    ("spread 1.5-3.5c", lambda s: 1.5 < s <= 3.5),
    ("spread > 3.5c", lambda s: s > 3.5),
    ("ALL", lambda s: True),
]


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "kalshi-mm-fv-backtest"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp)


def cents(dollars):
    return None if dollars is None else float(dollars) * 100.0


def discover_markets():
    ranked = []
    recent = int(time.time()) - DISCOVERY_UPDATED_WITHIN_HOURS * 3600
    cursor = ""
    for _ in range(DISCOVERY_PAGE_LIMIT):
        url = f"{BASE}/markets?limit=1000&min_updated_ts={recent}&mve_filter=exclude"
        if cursor:
            url += f"&cursor={cursor}"
        data = get(url)
        for market in data.get("markets", []):
            if market.get("status") != "active":
                continue
            volume = float(market.get("volume_24h_fp") or 0)
            if volume > 0:
                ranked.append((volume, market["ticker"]))
        cursor = data.get("cursor", "")
        if not cursor:
            break
    ranked.sort(reverse=True)
    return [ticker for _, ticker in ranked[:DISCOVERY_POOL_SIZE]]


def fetch_candles(ticker, hours):
    series = ticker.split("-")[0]
    end = int(time.time())
    start = end - hours * 3600
    url = (f"{BASE}/series/{series}/markets/{ticker}/candlesticks"
           f"?start_ts={start}&end_ts={end}&period_interval=1")
    try:
        return get(url).get("candlesticks", [])
    except Exception as err:
        print(f"  {ticker}: candle fetch failed ({err})", file=sys.stderr)
        return None


def parse(candle):
    price = candle.get("price") or {}
    yes_bid = candle.get("yes_bid") or {}
    yes_ask = candle.get("yes_ask") or {}
    return {
        "mean": cents(price.get("mean_dollars")),
        "bid_close": cents(yes_bid.get("close_dollars")),
        "ask_close": cents(yes_ask.get("close_dollars")),
        "volume": float(candle.get("volume_fp") or 0),
    }


def run_market(candles):
    rows = [parse(c) for c in candles]
    tape = {h: [0.0, 0.0] for h in HALF_LIVES_MINUTES}
    last_trade = None
    events = []
    for idx, row in enumerate(rows[:-1]):
        for half_life, acc in tape.items():
            decay = 0.5 ** (1.0 / half_life)
            acc[0] *= decay
            acc[1] *= decay
        if row["volume"] > 0 and row["mean"] is not None:
            last_trade = row["mean"]
            for acc in tape.values():
                acc[0] += row["volume"] * row["mean"]
                acc[1] += row["volume"]
        nxt = rows[idx + 1]
        if nxt["volume"] <= 0 or nxt["mean"] is None:
            continue
        if row["bid_close"] is None or row["ask_close"] is None:
            continue
        mid = (row["bid_close"] + row["ask_close"]) / 2.0
        cands = {"mid": mid}
        if last_trade is not None:
            cands["last"] = last_trade
        for half_life, acc in tape.items():
            cands[f"tape(h={half_life:g})"] = (
                acc[0] / acc[1] if acc[1] > MIN_TAPE_WEIGHT else mid)
        base_tape = cands[f"tape(h={BLEND_HALF_LIFE_MINUTES:g})"]
        for weight in BLEND_WEIGHTS:
            cands[f"blend(w={weight},h={BLEND_HALF_LIFE_MINUTES:g})"] = (
                weight * base_tape + (1 - weight) * mid)
        spread = row["ask_close"] - row["bid_close"]
        events.append((cands, nxt["mean"], spread))
    return events


def print_bucket(label, subset, names):
    print(f"--- {label}  (n={len(subset)}) ---")
    print(f"{'candidate':<24}{'n':>6}{'MAE (c)':>10}{'bias (c)':>10}")
    results = []
    for name in names:
        errs = [c[name] - nxt for c, nxt in subset if name in c]
        if not errs:
            continue
        mae = sum(abs(e) for e in errs) / len(errs)
        bias = sum(errs) / len(errs)
        results.append((mae, name, len(errs), bias))
    for mae, name, count, bias in sorted(results):
        print(f"{name:<24}{count:>6}{mae:>10.3f}{bias:>+10.3f}")
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tickers", nargs="*",
                        help="markets to score (default: auto-discover by 24h volume)")
    parser.add_argument("--hours", type=int, default=8,
                        help="candle lookback window (default 8)")
    parser.add_argument("--max-markets", type=int, default=30,
                        help="markets to include (default 30)")
    parser.add_argument("--min-traded-candles", type=int, default=20,
                        help="min traded minutes for a market to qualify (default 20)")
    args = parser.parse_args()

    tickers = args.tickers
    if not tickers:
        print("discovering active markets by 24h volume...", file=sys.stderr)
        tickers = discover_markets()
        print(f"{len(tickers)} candidates; probing candles...", file=sys.stderr)

    all_events = []
    used = []
    for ticker in tickers:
        if len(used) >= args.max_markets:
            break
        candles = fetch_candles(ticker, args.hours)
        time.sleep(0.15)
        if not candles:
            continue
        traded = sum(1 for c in candles if float(c.get("volume_fp") or 0) > 0)
        if traded < args.min_traded_candles:
            continue
        events = run_market(candles)
        if events:
            used.append(ticker)
            all_events.extend(events)
            print(f"  {ticker}: {traded} traded minutes, {len(events)} scoring events",
                  file=sys.stderr)

    if not all_events:
        print("no scoring events — no qualifying markets in the window")
        return

    names = sorted({name for cands, _, _ in all_events for name in cands})
    print(f"\nmarkets={len(used)} scoring_events={len(all_events)} "
          f"(1-min candles, last {args.hours}h, {time.strftime('%Y-%m-%d %H:%M %Z')})\n")
    for label, keep in SPREAD_BUCKETS:
        subset = [(c, n) for c, n, s in all_events if keep(s)]
        if subset:
            print_bucket(label, subset, names)


if __name__ == "__main__":
    main()
