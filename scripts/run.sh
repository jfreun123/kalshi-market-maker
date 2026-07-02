#!/usr/bin/env bash
# One shot: scan for markets, then immediately trade the result.
# Usage: scripts/run.sh [CONFIG] [--paper] [--build]
#   CONFIG   base config with credentials (default: config.json)
#   --paper  simulate fills locally, place no live orders
#   --build  force a rebuild of kalshi_mm before running
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

CONFIG="config.json"
PAPER=""
FORCE_BUILD=""
for arg in "$@"; do
  case "$arg" in
    --paper) PAPER="--paper" ;;
    --build|-b) FORCE_BUILD="force" ;;
    -h|--help) grep '^# ' "$0" | sed 's/^# //'; exit 0 ;;
    -*) die "unknown option: $arg" ;;
    *) CONFIG="$arg" ;;
  esac
done

cd "$REPO_ROOT"
ensure_binary "$FORCE_BUILD"
preflight_config "$CONFIG"

TRADE_CFG="$(trade_config_path "$CONFIG")"
info "[1/2] scanning markets (config: $CONFIG) ..."
"$BINARY" --scan "$CONFIG"
preflight_trade_config "$TRADE_CFG"

info "[2/2] starting market maker (config: $TRADE_CFG) ..."
if [[ -n "$PAPER" ]]; then
  info "PAPER mode — fills are simulated, no live orders"
else
  warn "LIVE mode — this WILL place real orders on the exchange"
fi
exec "$BINARY" ${PAPER:+$PAPER} "$TRADE_CFG"
