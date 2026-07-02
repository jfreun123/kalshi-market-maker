#!/usr/bin/env bash
# Start the market maker against a scanned trade config.
# Usage: scripts/trade.sh [TRADE_CONFIG] [--paper] [--build]
#   TRADE_CONFIG  config produced by scan.sh (default: config.trade.json)
#   --paper       simulate fills locally, place no live orders
#   --build       force a rebuild of kalshi_mm before running
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

TRADE_CONFIG=""
PAPER=""
FORCE_BUILD=""
for arg in "$@"; do
  case "$arg" in
    --paper) PAPER="--paper" ;;
    --build|-b) FORCE_BUILD="force" ;;
    -h|--help) grep '^# ' "$0" | sed 's/^# //'; exit 0 ;;
    -*) die "unknown option: $arg" ;;
    *) TRADE_CONFIG="$arg" ;;
  esac
done

cd "$REPO_ROOT"
TRADE_CONFIG="${TRADE_CONFIG:-config.trade.json}"
ensure_binary "$FORCE_BUILD"
preflight_config "$TRADE_CONFIG"
preflight_trade_config "$TRADE_CONFIG"

if [[ -n "$PAPER" ]]; then
  info "PAPER mode — fills are simulated, no live orders"
else
  warn "LIVE mode — this WILL place real orders on the exchange"
fi
info "starting market maker (config: $TRADE_CONFIG) — Ctrl-C stops and cancels quotes"
exec "$BINARY" ${PAPER:+$PAPER} "$TRADE_CONFIG"
