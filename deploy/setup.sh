#!/usr/bin/env bash
#
# Provision a fresh Ubuntu 24.04 LTS box to build, test, and run the Kalshi
# market maker. Idempotent — safe to re-run. Mirrors .github/workflows/ci.yml
# so a server build matches CI exactly.
#
# Usage:  bash deploy/setup.sh
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "==> [1/6] Installing build dependencies (apt)"
sudo apt-get update -q
sudo apt-get install -y \
  build-essential \
  clang clang-tidy clang-format cppcheck \
  cmake ninja-build \
  libssl-dev pkg-config \
  git lcov llvm

echo "==> [2/6] Configuring (cmake --preset dev)"
# Pulls the 6 FetchContent deps from github.com — needs outbound HTTPS.
cmake --preset dev

echo "==> [3/6] Building (cmake --build --preset dev)"
cmake --build --preset dev --parallel

echo "==> [4/6] Running test suite (ctest)"
ctest --preset dev --output-on-failure

echo "==> [5/6] Installing git pre-commit hook"
bash scripts/install-hooks.sh

echo "==> [6/6] Ensuring runtime logs/ directory exists"
mkdir -p "$REPO_ROOT/logs"

cat <<EOF

✅ Build complete: $REPO_ROOT/build/source/kalshi_mm

Next steps:
  1. Create config.json:   cp config.example.json config.json   (then edit it)
  2. Put your RSA private key on the box and point config.json at its path.
  3. Smoke test against demo / paper:
       ./build/source/kalshi_mm --paper config.json
  4. Install the systemd services (see deploy/README.md):
       deploy/kalshi-mm.service   — the trading bot
       deploy/claude-rc.service   — Claude Code remote control

NEVER commit config.json or the .pem key. Both are gitignored.
EOF
