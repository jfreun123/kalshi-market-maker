#!/usr/bin/env bash
# Opportunistic session harvester: demo has quotable flow only in narrow
# windows, so instead of scheduling sessions blind, poll the scanner and act
# only when the admission gates find real markets. >=4 admitted -> parallel
# A/B pair (model evidence + pnl); 1-3 -> single self-selecting session
# (pnl + self-recorded corpus via record_sessions); 0 -> sleep. Sessions
# self-record, so every harvested window automatically grows the backtest
# corpus. Usage: scripts/run_when_live.sh [hours] [check_minutes]  (12, 20)
set -euo pipefail
cd "$(dirname "$0")/.."

HOURS="${1:-12}"
CHECK_MINUTES="${2:-20}"
BIN="build/source/kalshi_mm"
DEADLINE=$(( $(date +%s) + HOURS * 3600 ))

echo "harvester: polling every ${CHECK_MINUTES}m for ${HOURS}h"
while (( $(date +%s) < DEADLINE )); do
  if pgrep -f "kalshi_mm" > /dev/null; then
    echo "harvester: $(date '+%H:%M') kalshi_mm already running — waiting"
    sleep $((CHECK_MINUTES * 60))
    continue
  fi
  rm -f scan_results.json
  "$BIN" --scan config-demo.json > /dev/null 2>&1 || true
  ADMITTED=0
  if [[ -f scan_results.json ]]; then
    ADMITTED=$(python3 -c "import json;print(len(json.load(open('scan_results.json')).get('markets',[])))" 2>/dev/null || echo 0)
  fi
  echo "harvester: $(date '+%H:%M') scan admitted ${ADMITTED} market(s)"
  if (( ADMITTED >= 4 )); then
    echo "harvester: launching A/B pair"
    scripts/run_ab.sh 20 || echo "harvester: pair failed — continuing"
  elif (( ADMITTED >= 1 )); then
    echo "harvester: launching single session"
    scripts/run_demo.sh 20 || echo "harvester: session failed — continuing"
  else
    sleep $((CHECK_MINUTES * 60))
  fi
done
echo "harvester: deadline reached"
