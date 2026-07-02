# Shared build + preflight helpers, sourced by scan.sh / trade.sh / run.sh.
# Every entry script cd's to REPO_ROOT first, so config paths are repo-relative.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BINARY="$REPO_ROOT/build/source/kalshi_mm"
DEFAULT_PROD_REST="https://api.elections.kalshi.com/trade-api/v2"

if [[ -t 1 ]]; then
  C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_GRN=$'\033[32m'; C_OFF=$'\033[0m'
else
  C_RED=""; C_YEL=""; C_GRN=""; C_OFF=""
fi

info() { printf '%s %s\n' "${C_GRN}▶${C_OFF}" "$*"; }
warn() { printf '%s %s\n' "${C_YEL}!${C_OFF}" "$*" >&2; }
die()  { printf '%s %s\n' "${C_RED}✗${C_OFF}" "$*" >&2; exit 1; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required tool '$1' not found${2:+ — $2}"
}

ensure_binary() {
  local force="${1:-}"
  require_cmd cmake "install CMake (brew install cmake)"
  require_cmd ninja "install Ninja (brew install ninja)"
  if [[ "$force" == "force" || ! -x "$BINARY" ]]; then
    info "building kalshi_mm ..."
    [[ -d "$REPO_ROOT/build" ]] || cmake --preset=dev >/dev/null ||
      die "cmake configure failed — run 'cmake --preset=dev' manually to see why"
    cmake --build "$REPO_ROOT/build" --target kalshi_mm ||
      die "build failed — fix the compile errors above and re-run"
  fi
  [[ -x "$BINARY" ]] || die "binary still missing at $BINARY after build"
}

preflight_config() {
  local cfg="$1"
  require_cmd jq "install jq (brew install jq)"
  [[ -f "$cfg" ]] ||
    die "config not found: $cfg — copy config.example.json to $cfg and fill it in"
  jq empty "$cfg" 2>/dev/null || die "config is not valid JSON: $cfg"

  local api_key key_path base_url
  api_key="$(jq -r '.api_key // empty' "$cfg")"
  key_path="$(jq -r '.private_key_path // empty' "$cfg")"
  base_url="$(jq -r ".base_url // \"$DEFAULT_PROD_REST\"" "$cfg")"

  [[ -n "$api_key" ]] || die "$cfg: 'api_key' is missing or empty"
  [[ "$api_key" != "YOUR_KALSHI_API_KEY" ]] ||
    die "$cfg: 'api_key' is still the placeholder from config.example.json"
  [[ -n "$key_path" ]] || die "$cfg: 'private_key_path' is missing or empty"
  [[ -f "$key_path" ]] ||
    die "private key file not found: $key_path (referenced by $cfg)"
  [[ -r "$key_path" ]] || die "private key file not readable: $key_path"
  grep -q "PRIVATE KEY" "$key_path" 2>/dev/null ||
    die "private key file is not a PEM (no 'PRIVATE KEY' marker): $key_path"

  if [[ "$base_url" == *demo* ]]; then
    info "environment: DEMO ($base_url)"
  else
    warn "base_url points at PRODUCTION ($base_url) — REAL orders, REAL money."
    warn "for a PnL POC use a demo config (base_url containing 'demo')."
  fi
}

trade_config_path() {
  local cfg="$1" dir base stem ext
  dir="$(dirname "$cfg")"; base="$(basename "$cfg")"
  stem="${base%.*}"; ext="${base##*.}"
  if [[ "$base" == "$ext" ]]; then
    printf '%s\n' "$dir/$base.trade"
  else
    printf '%s\n' "$dir/$stem.trade.$ext"
  fi
}

preflight_trade_config() {
  local cfg="$1"
  [[ -f "$cfg" ]] ||
    die "trade config not found: $cfg — run ./scripts/scan.sh first"
  jq empty "$cfg" 2>/dev/null || die "trade config is not valid JSON: $cfg"
  local count
  count="$(jq -r '.target_tickers // [] | length' "$cfg")"
  [[ "$count" -gt 0 ]] ||
    die "$cfg has no target_tickers — the scan matched nothing to quote"
  info "trade config $cfg → $count ticker(s)"
}
