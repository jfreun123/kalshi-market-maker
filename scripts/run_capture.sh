#!/usr/bin/env bash
# Record a live demo session for the fair-value backtest, then score it
# (BETTER_PRICING.md Phase 3b). The capture self-selects live markets when
# config target_tickers is empty; listen-only, no orders are placed.
# Usage: scripts/run_capture.sh [config] [minutes]
#   config  — default config-demo.json
#   minutes — stop automatically after N minutes (default: run until Ctrl-C)
# Output: capture/<stamp>/session.jsonl, rest.jsonl, and fv_scores.txt
set -euo pipefail
cd "$(dirname "$0")/.."

CONFIG="${1:-config-demo.json}"
MINUTES="${2:-}"
STAMP="$(date +%Y%m%d-%H%M%S)"
DIR="capture/$STAMP"
BIN="build/source/kalshi_mm"

if [[ ! -x "$BIN" ]]; then
  echo "run_capture: building..."
  cmake --build build
fi
if [[ ! -f "$CONFIG" ]]; then
  echo "run_capture: missing config: $CONFIG" >&2
  exit 1
fi

echo "run_capture: recording to $DIR (config=$CONFIG${MINUTES:+, ${MINUTES}m})"
"$BIN" --capture "$DIR" "$CONFIG" &
BOT_PID=$!
trap 'kill -INT "$BOT_PID" 2>/dev/null || true' INT TERM
TIMER_PID=""
if [[ -n "$MINUTES" ]]; then
  (sleep $((MINUTES * 60)) && kill -INT "$BOT_PID" 2>/dev/null) &
  TIMER_PID=$!
fi
wait "$BOT_PID" || true
[[ -n "$TIMER_PID" ]] && kill "$TIMER_PID" 2>/dev/null || true

if [[ ! -s "$DIR/session.jsonl" ]]; then
  echo "run_capture: no frames recorded in $DIR/session.jsonl" >&2
  exit 1
fi

FRAMES=$(wc -l < "$DIR/session.jsonl" | tr -d ' ')
TRADES=$(grep -c '"type":"trade"' "$DIR/session.jsonl" || true)
echo "run_capture: $FRAMES frames, $TRADES public trades"
echo
echo "== fair-value scoreboard =="
"$BIN" --fv-replay "$DIR/session.jsonl" | tee "$DIR/fv_scores.txt"
echo
echo "run_capture: scoreboard saved to $DIR/fv_scores.txt"
