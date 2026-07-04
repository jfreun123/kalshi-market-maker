#!/usr/bin/env bash
# Scan for live markets, then run a timed demo session with a clean SIGINT
# flatten. Usage: scripts/run_demo.sh [--clean] [minutes]   (default 5)
# --clean archives logs/*.log and logs/*.jsonl to logs/archive/<stamp>/ so
# every session starts with fresh logs; nothing is deleted.
set -euo pipefail
cd "$(dirname "$0")/.."

MINUTES=5
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    *) MINUTES="$arg" ;;
  esac
done

if [[ "$CLEAN" -eq 1 ]]; then
  stamp=$(date +%Y%m%d-%H%M%S)
  mkdir -p "logs/archive/$stamp"
  find logs -maxdepth 1 -type f \( -name "*.log*" -o -name "*.jsonl" \) \
    -exec mv {} "logs/archive/$stamp/" \; 2>/dev/null || true
  echo "run_demo: logs archived to logs/archive/$stamp"
fi

./build/source/kalshi_mm --scan config-demo.json
./build/source/kalshi_mm config-demo.trade.json &
BOT_PID=$!
trap 'kill -INT "$BOT_PID" 2>/dev/null || true' INT TERM
(sleep $((MINUTES * 60)) && kill -INT "$BOT_PID" 2>/dev/null) &
TIMER_PID=$!
wait "$BOT_PID"
kill "$TIMER_PID" 2>/dev/null || true
echo "run_demo: session ended after <= $MINUTES minute(s)"
