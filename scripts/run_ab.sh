#!/usr/bin/env bash
# Run a parallel fair-value A/B: two sessions in the SAME minutes on disjoint
# market sets — leg A heuristic, leg B clearing (BETTER_PRICING Phase 5).
# Scans once, splits the top 4 picks rank-balanced (A gets 1&4, B gets 2&3),
# scopes each leg's order janitorial and pnl state to itself, and prints both
# attributions at the end. --swap gives leg A the clearing model instead
# (alternate across pairs so market-set luck cancels out).
# Usage: scripts/run_ab.sh [minutes] [--swap]     (default 15)
set -euo pipefail
cd "$(dirname "$0")/.."

MINUTES=15
SWAP=0
for arg in "$@"; do
  case "$arg" in
    --swap) SWAP=1 ;;
    *) MINUTES="$arg" ;;
  esac
done

BIN="build/source/kalshi_mm"
STAMP="$(date +%Y%m%d-%H%M%S)"
AB_DIR="logs/ab-$STAMP"
mkdir -p "$AB_DIR"

echo "run_ab: scanning for live markets..."
rm -f scan_results.json
"$BIN" --scan config-demo.json > "$AB_DIR/scan.log" 2>&1
if [[ ! -f scan_results.json ]]; then
  echo "run_ab: scan produced no results file — aborting" >&2
  exit 1
fi
SWAP="$SWAP" AB_DIR="$AB_DIR" python3 - <<'EOF'
import json, os

swap = os.environ["SWAP"] == "1"
ab_dir = os.environ["AB_DIR"]
scan = json.load(open("scan_results.json"))
tickers = [m["ticker"] for m in scan["markets"][:4]]
if len(tickers) < 4:
    raise SystemExit(f"run_ab: only {len(tickers)} markets passed the scan — need 4")
set_a, set_b = [tickers[0], tickers[3]], [tickers[1], tickers[2]]

base = json.load(open("config-demo.json"))
if "secrets_path" in base:
    base["secrets_path"] = os.path.abspath(base["secrets_path"])
for leg, tick_set, clearing in (
    ("A", set_a, swap),
    ("B", set_b, not swap),
):
    cfg = dict(base)
    cfg["target_tickers"] = tick_set
    cfg["log_dir"] = f"{ab_dir}/{leg}"
    cfg["pnl_state_path"] = f"{ab_dir}/{leg}/pnl_state.json"
    cfg["account_wide_janitorial"] = False
    quoter = dict(cfg.get("quoter", {}))
    quoter["use_clearing_pricing"] = clearing
    cfg["quoter"] = quoter
    json.dump(cfg, open(f"{ab_dir}/config-{leg}.json", "w"), indent=1)
    model = "clearing" if clearing else "heuristic"
    print(f"run_ab: leg {leg} = {model} on {tick_set}")
EOF

"$BIN" "$AB_DIR/config-A.json" > "$AB_DIR/A.out" 2>&1 &
PID_A=$!
"$BIN" "$AB_DIR/config-B.json" > "$AB_DIR/B.out" 2>&1 &
PID_B=$!
trap 'kill -TERM "$PID_A" "$PID_B" 2>/dev/null || true' INT TERM
echo "run_ab: both legs live for $MINUTES minute(s) (pids $PID_A $PID_B)"
(sleep $((MINUTES * 60)) && kill -TERM "$PID_A" "$PID_B" 2>/dev/null) &
TIMER_PID=$!
wait "$PID_A" || true
wait "$PID_B" || true
kill "$TIMER_PID" 2>/dev/null || true

echo
for leg in A B; do
  echo "== leg $leg attribution =="
  grep -m1 "pricing model" "$AB_DIR/$leg.out" || true
  python3 scripts/pnl_attribution.py "$AB_DIR/$leg"/analytics_*.jsonl 2>/dev/null | tail -6 || echo "  (no fills)"
  echo
done
echo "run_ab: logs and configs in $AB_DIR"
