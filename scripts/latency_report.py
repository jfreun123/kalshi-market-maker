#!/usr/bin/env python3
"""L0 latency report over analytics JSONL files.

Usage: python3 scripts/latency_report.py logs/analytics_*.jsonl

Groups type=="http" events by method+endpoint class and prints
count / p50 / p95 / max round-trip milliseconds. This is the baseline every
infra change (VM, amend, async dispatch) is judged against — same script,
same stages, before and after.
"""

import json
import sys
from collections import defaultdict


def endpoint_class(method, path):
    if "/portfolio/orders" in path:
        return f"{method} orders (place/cancel)"
    if "/portfolio/fills" in path:
        return f"{method} fills"
    if "/portfolio/positions" in path:
        return f"{method} positions"
    if "/markets/trades" in path:
        return f"{method} trades-probe"
    if "/orderbook" in path:
        return f"{method} orderbook"
    return f"{method} other"


def percentile(sorted_values, fraction):
    if not sorted_values:
        return None
    index = min(len(sorted_values) - 1, int(fraction * len(sorted_values)))
    return sorted_values[index]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    samples = defaultdict(list)
    for path in sys.argv[1:]:
        with open(path) as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                event = json.loads(line)
                if event.get("type") != "http":
                    continue
                samples[endpoint_class(event["method"], event["path"])].append(
                    event["rtt_ms"]
                )
    if not samples:
        print("no http latency events found — run a session first")
        return 0
    print(f"{'endpoint':<32} {'n':>6} {'p50':>7} {'p95':>7} {'max':>7}")
    for name in sorted(samples):
        values = sorted(samples[name])
        print(f"{name:<32} {len(values):>6} {percentile(values, 0.50):>5}ms "
              f"{percentile(values, 0.95):>5}ms {values[-1]:>5}ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
