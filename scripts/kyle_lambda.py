#!/usr/bin/env python3
"""Kyle lambda price-impact study over session logs (PLAN item 80).

Usage: python3 scripts/kyle_lambda.py logs/session_frames.jsonl \
    logs/analytics.jsonl [more.jsonl ...] [--bar-ms 60000] [--jump-cents 3]

Per market, mid changes (from analytics quote records, last observation per
bar) are regressed on signed net taker flow (from WS trade frames, YES-buy
positive contracts) over fixed bars: Delta mid = lambda * Q + c. Kyle's
lambda is the adverse-selection tax in cents per contract of net flow —
the theoretically correct scale for a dynamic spread floor (widen when
lambda rises, i.e. when fundamental uncertainty is high relative to noise
volume, before any imbalance shows).

After the largest |Delta mid| jump bar of at least --jump-cents, lambda is
re-estimated on the bars before and after the jump. The trajectory
discriminates the two informed-flow regimes (Krause Ch.2): a monopolist
insider trades smoothly — lambda stays stable after a jump, danger persists
all session, stay wide; competing insiders spend the information in one
burst — lambda collapses after the jump and re-tightening is safe.
Trades are deduplicated by trade_id across overlapping captures.
"""

import argparse
import json
import math
from collections import defaultdict

BAR_MS_DEFAULT = 60_000
JUMP_CENTS_DEFAULT = 3.0
MIN_BARS = 20
MIN_ACTIVE_BARS = 8
REGIME_WINDOW_BARS = 15
COLLAPSE_FRACTION = 0.5
MIN_REGIME_BARS = 6
REGIME_MIN_T = 2.0
CONTRACTS_PER_LOT = 100.0


def load_records(paths):
    trades_by_market, quotes_by_ticker = defaultdict(dict), defaultdict(list)
    for path in paths:
        with open(path) as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                record = json.loads(line)
                kind = record.get("type")
                if kind == "trade" and "msg" in record:
                    message = record["msg"]
                    signed = float(message["count_fp"]) * (
                        1.0 if message["taker_side"] == "yes" else -1.0
                    )
                    trades_by_market[message["market_ticker"]][
                        message["trade_id"]
                    ] = (message["ts_ms"], signed)
                elif kind == "quote" and "ticker" in record:
                    quotes_by_ticker[record["ticker"]].append(
                        (record["ts_ms"], record["mid"])
                    )
    for series in quotes_by_ticker.values():
        series.sort()
    return trades_by_market, quotes_by_ticker


def build_bars(trades, quotes, bar_ms):
    if not quotes:
        return []
    start_ts = quotes[0][0]
    boundary_count = int((quotes[-1][0] - start_ts) // bar_ms) + 1
    flow = [0.0] * (boundary_count + 1)
    for ts_ms, signed in trades:
        offset = ts_ms - start_ts
        if offset <= 0 or offset > boundary_count * bar_ms:
            continue
        flow[int(math.ceil(offset / bar_ms))] += signed
    mid_at_boundary, quote_index, last_mid = [], 0, None
    for index in range(boundary_count + 1):
        boundary = start_ts + index * bar_ms
        while quote_index < len(quotes) and quotes[quote_index][0] <= boundary:
            last_mid = quotes[quote_index][1]
            quote_index += 1
        mid_at_boundary.append(last_mid)
    bars = []
    for index in range(1, boundary_count + 1):
        previous_mid = mid_at_boundary[index - 1]
        if mid_at_boundary[index] is not None and previous_mid is not None:
            bars.append((mid_at_boundary[index] - previous_mid, flow[index]))
    return bars


def regress(bars):
    active = [bar for bar in bars if bar[1] != 0.0]
    if len(bars) < MIN_REGIME_BARS or len(active) < 2:
        return None
    n = len(bars)
    mean_flow = sum(flow for _, flow in bars) / n
    mean_change = sum(change for change, _ in bars) / n
    var_flow = sum((flow - mean_flow) ** 2 for _, flow in bars) / n
    if var_flow == 0:
        return None
    slope = sum(
        (flow - mean_flow) * (change - mean_change) for change, flow in bars
    ) / (n * var_flow)
    intercept = mean_change - slope * mean_flow
    residual_var = sum(
        (change - intercept - slope * flow) ** 2 for change, flow in bars
    ) / n
    var_change = sum((change - mean_change) ** 2 for change, _ in bars) / n
    r_squared = 1.0 - residual_var / var_change if var_change > 0 else 0.0
    degrees = n - 2
    if degrees <= 0 or residual_var == 0:
        t_stat = float("inf") if slope != 0 else 0.0
    else:
        standard_error = math.sqrt(residual_var / degrees / var_flow)
        t_stat = slope / standard_error if standard_error > 0 else 0.0
    return {
        "lambda": slope,
        "t": t_stat,
        "r2": r_squared,
        "n": n,
        "active": len(active),
    }


def classify_jump(bars, jump_cents):
    changes = [abs(change) for change, _ in bars]
    if not changes:
        return None
    jump_index = max(range(len(bars)), key=lambda index: changes[index])
    jump_size = changes[jump_index]
    if jump_size < jump_cents:
        return None
    before = bars[max(0, jump_index - REGIME_WINDOW_BARS):jump_index]
    after = bars[jump_index + 1:jump_index + 1 + REGIME_WINDOW_BARS]
    fit_before = regress(before) if len(before) >= MIN_REGIME_BARS else None
    fit_after = regress(after) if len(after) >= MIN_REGIME_BARS else None
    if fit_before is None or fit_after is None:
        return {"jump": jump_size, "label": "jump (windows too thin)"}
    lambda_before, lambda_after = fit_before["lambda"], fit_after["lambda"]
    if lambda_before <= 0 or fit_before["t"] < REGIME_MIN_T:
        label = "jump (pre-lambda insignificant)"
    elif lambda_after < COLLAPSE_FRACTION * lambda_before:
        label = "collapsed (burst spent - re-tighten ok)"
    else:
        label = "stable (persistent informed - stay wide)"
    return {
        "jump": jump_size,
        "label": label,
        "before": lambda_before,
        "after": lambda_after,
    }


def fmt(value, precision=4):
    return "-" if value is None else f"{value:.{precision}f}"


def main():
    parser = argparse.ArgumentParser(
        description="Kyle lambda impact study (PLAN item 80)"
    )
    parser.add_argument("files", nargs="+")
    parser.add_argument("--bar-ms", type=int, default=BAR_MS_DEFAULT)
    parser.add_argument("--jump-cents", type=float, default=JUMP_CENTS_DEFAULT)
    arguments = parser.parse_args()

    trades_by_market, quotes_by_ticker = load_records(arguments.files)
    header = (
        f"{'market':<42} {'bars':>5} {'actv':>5} {'lam/100':>8} {'t':>6} "
        f"{'r2':>5}  jump-regime"
    )
    print(
        f"=== Kyle lambda (bar={arguments.bar_ms}ms, cents per 100 contracts "
        f"of net taker flow) ==="
    )
    print(header)
    print("-" * len(header))
    for ticker in sorted(quotes_by_ticker):
        trades = sorted(trades_by_market.get(ticker, {}).values())
        bars = build_bars(trades, quotes_by_ticker[ticker], arguments.bar_ms)
        if len(bars) < MIN_BARS:
            continue
        fit = regress(bars)
        if fit is None or fit["active"] < MIN_ACTIVE_BARS:
            continue
        jump = classify_jump(bars, arguments.jump_cents)
        if jump is None:
            regime = "no jump"
        elif "before" in jump:
            regime = (
                f"{jump['label']} (jump {jump['jump']:.1f}c, "
                f"{jump['before'] * CONTRACTS_PER_LOT:.3f} -> "
                f"{jump['after'] * CONTRACTS_PER_LOT:.3f})"
            )
        else:
            regime = f"{jump['label']} (jump {jump['jump']:.1f}c)"
        print(
            f"{ticker:<42} {fit['n']:>5} {fit['active']:>5} "
            f"{fmt(fit['lambda'] * CONTRACTS_PER_LOT, 3):>8} "
            f"{fit['t']:>6.1f} {fit['r2']:>5.2f}  {regime}"
        )


if __name__ == "__main__":
    main()
