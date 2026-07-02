#!/usr/bin/env bash
# Scan Kalshi markets and write a ready-to-trade config.
# Usage: scripts/scan.sh [CONFIG] [--build]
#   CONFIG   base config with credentials (default: config.json)
#   --build  force a rebuild of kalshi_mm before running
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

CONFIG="config.json"
FORCE_BUILD=""
for arg in "$@"; do
  case "$arg" in
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
info "scanning markets (config: $CONFIG) ..."
"$BINARY" --scan "$CONFIG"

[[ -f "$TRADE_CFG" ]] ||
  die "scan finished but no trade config was written to $TRADE_CFG"
info "wrote $TRADE_CFG — start the maker with: ./scripts/trade.sh $TRADE_CFG"
