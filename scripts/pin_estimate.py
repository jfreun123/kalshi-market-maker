#!/usr/bin/env python3
"""PIN (probability of informed trading) per series from the WS tape
(PLAN item 81).

Usage: python3 scripts/pin_estimate.py logs/session_frames.jsonl \
    [more.jsonl ...] [--window-ms 900000] [--min-windows 20]

Easley-Kiefer-O'Hara-Paperman mixture: each window either has no
information event (prob 1-alpha; buys ~ Pois(eps_b), sells ~ Pois(eps_s)),
a bad-news event (prob alpha*delta; informed rate mu joins the sell side)
or a good-news event (informed rate joins the buy side).
PIN = alpha*mu / (alpha*mu + eps_b + eps_s) — the probability any given
trade is informed. Kalshi's tape carries the exact taker side, removing
the trade-classification bias that plagues equity PIN estimates.

Windows are counted per market over its tape span (empty interior windows
count as (0,0)), then pooled per series (ticker prefix) for the MLE.
Maximum likelihood via Nelder-Mead on transformed parameters
(logit alpha/delta, log rates) with jittered restarts; deterministic seed.

The mixture is only reported when it beats the alpha=0 null (independent
Poisson buys/sells at the sample means) by a chi-square(3) likelihood-ratio
test AND alpha stays off the boundary — the known PIN degeneracy lets an
"event every window" solution absorb mu into the base rate on weak-signal
data, and a weak-signal series genuinely has PIN ~ 0. Rejected fits print
PIN 0.00 with flag "ns".
Use: market screen plus spread-floor scaler — high-PIN series should show
worse maker markouts (cross-check via analyze_fills.py).
"""

import argparse
import json
import math
import random
from collections import defaultdict

WINDOW_MS_DEFAULT = 900_000
MIN_WINDOWS_DEFAULT = 20
RESTARTS = 6
NELDER_MEAD_ITERATIONS = 1_200
SIMPLEX_STEP = 0.4
JITTER_SCALE = 0.8
CONVERGENCE_TOL = 1e-9
LOGIT_CLAMP = 12.0
LOG_RATE_CLAMP = 12.0
LIKELIHOOD_RATIO_THRESHOLD = 7.81
ALPHA_BOUNDARY = 0.95


def load_trades(paths):
    trades_by_market = defaultdict(dict)
    for path in paths:
        with open(path) as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                record = json.loads(line)
                if record.get("type") != "trade" or "msg" not in record:
                    continue
                message = record["msg"]
                trades_by_market[message["market_ticker"]][
                    message["trade_id"]
                ] = (message["ts_ms"], message["taker_side"] == "yes")
    return trades_by_market


def window_counts(trades_by_market, window_ms):
    counts_by_series = defaultdict(list)
    for ticker, prints in trades_by_market.items():
        ordered = sorted(prints.values())
        start_ts = ordered[0][0]
        span = ordered[-1][0] - start_ts
        window_count = int(span // window_ms) + 1
        windows = [[0, 0] for _ in range(window_count)]
        for ts_ms, is_buy in ordered:
            index = int((ts_ms - start_ts) // window_ms)
            windows[index][0 if is_buy else 1] += 1
        series = ticker.split("-")[0]
        counts_by_series[series].extend(
            (buys, sells) for buys, sells in windows
        )
    return counts_by_series


def sigmoid(value):
    clamped = max(-LOGIT_CLAMP, min(LOGIT_CLAMP, value))
    return 1.0 / (1.0 + math.exp(-clamped))


def positive(value):
    return math.exp(max(-LOG_RATE_CLAMP, min(LOG_RATE_CLAMP, value)))


def unpack(vector):
    return {
        "alpha": sigmoid(vector[0]),
        "delta": sigmoid(vector[1]),
        "mu": positive(vector[2]),
        "eps_b": positive(vector[3]),
        "eps_s": positive(vector[4]),
    }


def poisson_log(count, rate):
    return count * math.log(rate) - rate - math.lgamma(count + 1)


def log_sum_exp(values):
    peak = max(values)
    if peak == float("-inf"):
        return peak
    return peak + math.log(sum(math.exp(value - peak) for value in values))


def negative_log_likelihood(vector, windows):
    params = unpack(vector)
    alpha, delta = params["alpha"], params["delta"]
    mu, eps_b, eps_s = params["mu"], params["eps_b"], params["eps_s"]
    total = 0.0
    for buys, sells in windows:
        none_term = (
            math.log(max(1.0 - alpha, 1e-300))
            + poisson_log(buys, eps_b)
            + poisson_log(sells, eps_s)
        )
        bad_term = (
            math.log(max(alpha * delta, 1e-300))
            + poisson_log(buys, eps_b)
            + poisson_log(sells, eps_s + mu)
        )
        good_term = (
            math.log(max(alpha * (1.0 - delta), 1e-300))
            + poisson_log(buys, eps_b + mu)
            + poisson_log(sells, eps_s)
        )
        total -= log_sum_exp([none_term, bad_term, good_term])
    return total


def nelder_mead(objective, start, iterations):
    dimension = len(start)
    simplex = [list(start)]
    for axis in range(dimension):
        vertex = list(start)
        vertex[axis] += SIMPLEX_STEP
        simplex.append(vertex)
    scores = [objective(vertex) for vertex in simplex]
    for _ in range(iterations):
        order = sorted(range(len(simplex)), key=lambda i: scores[i])
        simplex = [simplex[i] for i in order]
        scores = [scores[i] for i in order]
        if abs(scores[-1] - scores[0]) < CONVERGENCE_TOL:
            break
        centroid = [
            sum(vertex[axis] for vertex in simplex[:-1]) / dimension
            for axis in range(dimension)
        ]
        worst = simplex[-1]
        reflected = [
            centroid[axis] + (centroid[axis] - worst[axis])
            for axis in range(dimension)
        ]
        reflected_score = objective(reflected)
        if reflected_score < scores[0]:
            expanded = [
                centroid[axis] + 2.0 * (centroid[axis] - worst[axis])
                for axis in range(dimension)
            ]
            expanded_score = objective(expanded)
            if expanded_score < reflected_score:
                simplex[-1], scores[-1] = expanded, expanded_score
            else:
                simplex[-1], scores[-1] = reflected, reflected_score
        elif reflected_score < scores[-2]:
            simplex[-1], scores[-1] = reflected, reflected_score
        else:
            contracted = [
                centroid[axis] + 0.5 * (worst[axis] - centroid[axis])
                for axis in range(dimension)
            ]
            contracted_score = objective(contracted)
            if contracted_score < scores[-1]:
                simplex[-1], scores[-1] = contracted, contracted_score
            else:
                best = simplex[0]
                simplex = [best] + [
                    [
                        best[axis] + 0.5 * (vertex[axis] - best[axis])
                        for axis in range(dimension)
                    ]
                    for vertex in simplex[1:]
                ]
                scores = [scores[0]] + [
                    objective(vertex) for vertex in simplex[1:]
                ]
    best_index = min(range(len(simplex)), key=lambda i: scores[i])
    return simplex[best_index], scores[best_index]


def logit(probability):
    return math.log(probability / (1.0 - probability))


def fit_series(windows, rng):
    mean_buys = sum(buys for buys, _ in windows) / len(windows)
    mean_sells = sum(sells for _, sells in windows) / len(windows)
    base_rate = max(0.5, (mean_buys + mean_sells) / 2.0)
    start = [
        logit(0.3),
        logit(0.5),
        math.log(max(0.5, abs(mean_buys - mean_sells) + base_rate)),
        math.log(max(0.25, 0.75 * mean_buys)),
        math.log(max(0.25, 0.75 * mean_sells)),
    ]
    objective = lambda vector: negative_log_likelihood(vector, windows)
    best_vector, best_score = None, float("inf")
    for restart in range(RESTARTS):
        seed = (
            start if restart == 0
            else [value + rng.gauss(0, JITTER_SCALE) for value in start]
        )
        vector, score = nelder_mead(objective, seed, NELDER_MEAD_ITERATIONS)
        if score < best_score:
            best_vector, best_score = vector, score
    params = unpack(best_vector)
    informed = params["alpha"] * params["mu"]
    params["pin"] = informed / (
        informed + params["eps_b"] + params["eps_s"]
    )
    null_score = -sum(
        poisson_log(buys, max(mean_buys, 1e-9))
        + poisson_log(sells, max(mean_sells, 1e-9))
        for buys, sells in windows
    )
    params["likelihood_ratio"] = 2.0 * (null_score - best_score)
    params["significant"] = (
        params["likelihood_ratio"] > LIKELIHOOD_RATIO_THRESHOLD
        and params["alpha"] <= ALPHA_BOUNDARY
    )
    return params


def main():
    parser = argparse.ArgumentParser(
        description="PIN per series from the WS tape (PLAN item 81)"
    )
    parser.add_argument("files", nargs="+")
    parser.add_argument("--window-ms", type=int, default=WINDOW_MS_DEFAULT)
    parser.add_argument(
        "--min-windows", type=int, default=MIN_WINDOWS_DEFAULT
    )
    arguments = parser.parse_args()

    trades_by_market = load_trades(arguments.files)
    counts_by_series = window_counts(trades_by_market, arguments.window_ms)
    rng = random.Random(0)
    header = (
        f"{'series':<18} {'wins':>5} {'B/win':>6} {'S/win':>6} {'alpha':>6} "
        f"{'delta':>6} {'mu':>7} {'eps_b':>7} {'eps_s':>7} {'LR':>6} "
        f"{'PIN':>5}  flag"
    )
    print(
        f"=== PIN per series (window={arguments.window_ms // 60000}min, "
        f"{sum(len(w) for w in counts_by_series.values())} windows) ==="
    )
    print(header)
    print("-" * len(header))
    results = []
    for series, windows in counts_by_series.items():
        if len(windows) < arguments.min_windows:
            continue
        params = fit_series(windows, rng)
        mean_buys = sum(buys for buys, _ in windows) / len(windows)
        mean_sells = sum(sells for _, sells in windows) / len(windows)
        results.append((series, len(windows), mean_buys, mean_sells, params))
    results.sort(
        key=lambda row: row[4]["pin"] if row[4]["significant"] else 0.0,
        reverse=True,
    )
    for series, window_count, mean_buys, mean_sells, params in results:
        reported_pin = params["pin"] if params["significant"] else 0.0
        flag = "" if params["significant"] else "ns"
        print(
            f"{series:<18} {window_count:>5} {mean_buys:>6.1f} "
            f"{mean_sells:>6.1f} {params['alpha']:>6.2f} "
            f"{params['delta']:>6.2f} {params['mu']:>7.2f} "
            f"{params['eps_b']:>7.2f} {params['eps_s']:>7.2f} "
            f"{params['likelihood_ratio']:>6.1f} {reported_pin:>5.2f}  {flag}"
        )
    skipped = [
        series for series, windows in counts_by_series.items()
        if len(windows) < arguments.min_windows
    ]
    if skipped:
        print(
            f"skipped (fewer than {arguments.min_windows} windows): "
            f"{', '.join(sorted(skipped))}"
        )


if __name__ == "__main__":
    main()
