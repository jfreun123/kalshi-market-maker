# kalshi-market-maker

A C++23 automated market maker for [Kalshi](https://kalshi.com) prediction markets.
Self-selects live markets at startup, quotes two-sided post-only markets around an
inventory-skewed micro-price, manages risk via a constraint bitset, and logs every
quote/fill/HTTP round trip to JSONL for offline analysis.

## Architecture

```
WebSocketClient ──► TradingSession ──► Quoter ──► OrderManager ──► RestClient ──► Kalshi API
                          │                │            │
                          │          FairValueEngine    └── RiskManager (constraint bitset
                          │          (IPricingModel)          + portfolio kill-switch)
                          ├── FlowImbalanceGuard
                          ├── AnalyticsLogger (JSONL: quotes, fills, HTTP RTT)
                          └── Portfolio (read-model) ──► RiskManager
```

Details and sequence diagrams: [docs/architecture.md](docs/architecture.md).
Every defensive mechanism, with provenance: [docs/GUARDS.md](docs/GUARDS.md).

**Key components:**

| Component | File | Responsibility |
|---|---|---|
| `TradingSession` | `trading_session.hpp` | Domain reactions (snapshot/delta/fill, portfolio kill-switch, market add/remove); the seam shared by prod, tests, and replay |
| `Quoter` | `quoter.hpp` | Two-sided quoting: EMA micro-price, flow lean, LMSR inventory skew, longshot floor, rest/fade timers, amend-first repricing, wind-down |
| `OrderManager` | `order_manager.hpp` | Open orders, positions, realized + unrealized PnL, fill dedup |
| `RiskManager` | `risk_manager.hpp` | Constraint bitset; blocks orders when any bit is set; portfolio checks are the global kill-switch |
| `Portfolio` | `portfolio.hpp` | Read-model: total PnL, capital-at-risk, per-event rollup |
| `RestClient` | `rest_client.hpp` | V2 REST: place/cancel/amend/decrease, fills, positions, rate-limited writes |
| `WebSocketClient` | `websocket_client.hpp` | Orderbook snapshots/deltas + fill events |
| `FlowImbalanceGuard` | `flow_imbalance.hpp` | Rolling-window one-sided-flow detector; drives spread widen + directional lean |
| `AnalyticsLogger` | `analytics.hpp` | JSONL event stream (`type=quote/fill/http`) for the analysis scripts |
| `TickerScanner` | `ticker_scanner.hpp` | Ranks open markets (volume, spread, liveness, incentive pools) for startup selection and rotation |
| `FairValueEngine` / models | `fair_value.hpp`, `pricing_model.hpp` | Pluggable `IPricingModel`: `HeuristicModel` (default) or `ViewBasedModel` (favorite-longshot debias) |
| `Auth` | `auth.hpp` | RSA-PSS-SHA256 request signing |
| App modes | `app_modes.hpp`, `cli.hpp` | `--scan/--reconcile/--flatten/--capture/--paper` runners; rotation |

## Prerequisites

**Ubuntu / Debian:**
```bash
sudo apt-get install cmake clang clang-tidy cppcheck ninja-build libssl-dev llvm lcov
```

**macOS (Homebrew):**
```bash
brew install cmake llvm cppcheck ninja openssl
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

All other dependencies (nlohmann/json, cpp-httplib, IXWebSocket, spdlog, GoogleTest, Google Benchmark) are fetched automatically by CMake via FetchContent.

## Building

```bash
bash scripts/install-hooks.sh    # pre-commit hook, once per clone
cmake --preset=dev
cmake --build --preset=dev
ctest --preset=dev
```

## Configuration

Copy `config.example.json` (it shows every knob at its default) and fill in
credentials:

```bash
cp config.example.json config.json
```

- `target_tickers` — normally leave **empty**: at startup the session scans all
  open markets and selects the top `scanner.trade_top_n` itself, then re-scans
  every `scanner.rotation_minutes` and swaps out markets that have gone dead
  (never ones holding a position or resting orders). Set tickers explicitly to
  pin specific markets.
- `quoter` — spread/size plus the guard knobs (rest timers, fade, longshot
  floor, flow lean, inventory cap, wind-down). Each is documented in
  [docs/GUARDS.md](docs/GUARDS.md); defaults live in `quoter.hpp`.
- `risk` — per-market and portfolio limits (exposure, total-loss, drawdown).
- `flow` — the imbalance guard's window and trigger ratio.
- `scanner` — market filters (price band, spread band, 24h volume, days to
  close, `max_stale_trade_minutes` liveness cutoff).

Config files with real credentials (`config.json`, `config-demo.json`) are
gitignored. Secrets policy: see `CLAUDE.md`.

Generate an RSA key pair for API authentication:

```bash
openssl genrsa -out kalshi-private-key.pem 2048
openssl rsa -in kalshi-private-key.pem -pubout -out kalshi-public-key.pem
# Upload the public key at kalshi.com → Settings → API Keys
```

## Running

```bash
# Live trading (markets self-selected unless target_tickers is set)
./build/source/kalshi_mm config.json

# Timed demo session with fresh logs (archives old logs, runs N minutes)
bash scripts/run_demo.sh --clean 5

# Research: scan and rank markets, write scan_results.json (no orders)
./build/source/kalshi_mm --scan config.json

# Paper trading (simulated fills, no real orders)
./build/source/kalshi_mm --paper config.json

# One-shot accounting check vs the exchange (exit non-zero on drift)
./build/source/kalshi_mm --reconcile config.json

# Close all open positions and exit
./build/source/kalshi_mm --flatten config.json

# Record a live session for replay (no orders; Ctrl-C to stop)
./build/source/kalshi_mm --capture capture/demo-run config-demo.json
```

Demo environment: point `base_url` / `ws_url` at
`https://demo-api.kalshi.co/trade-api/v2` and
`wss://demo-api.kalshi.co/trade-api/ws/v2` (see `docs/KALSHI_API_REFERENCE.md`
for all hosts). Production and demo credentials are not interchangeable.

### Session hygiene

Startup cancels every resting order account-wide (no zombies from a prior
run). Shutdown goes reduce-only for up to `quoter.winddown_seconds` to exit
inventory as a maker, then cancels all quotes (verified against the exchange,
3 attempts) and taker-flattens any remainder — the bot always ends flat.
Every ~2 minutes local accounting is reconciled against the exchange's
positions; any drift halts quoting (`kModelDiverge`).

### Analysis scripts

Each session writes `logs/analytics_<date>.jsonl` (quote decisions, fills with
fees and inventory, HTTP RTTs). Offline:

```bash
python3 scripts/analyze_fills.py logs/analytics_*.jsonl     # markout @30s/@5min, effective spread
python3 scripts/pnl_attribution.py logs/analytics_*.jsonl   # entry_edge / drift / exit_cost + picked-off detector
python3 scripts/latency_report.py logs/analytics_*.jsonl    # p50/p95/max RTT per endpoint class
```

### Portfolio risk

`OrderManager` is the single source of truth; `Portfolio` is a read-model
aggregating realized + unrealized PnL, capital at risk, and per-event rollups.
`RiskManager::update_portfolio()` consumes it (~1s cadence) as a global
kill-switch: `kOverExposure` (capital at risk > `max_total_exposure_dollars`),
`kPortfolioLoss` (total PnL incl. mark-to-market < `max_total_loss_dollars`),
`kDrawdown` (give-back from the session high-water mark >
`max_drawdown_dollars`). All three halt every quoter and require a manual
`resume()`.

## Development

| Preset | Purpose |
|---|---|
| `dev` | Debug build with clang-tidy + cppcheck |
| `asan` / `tsan` | AddressSanitizer / ThreadSanitizer |
| `coverage` | Line coverage via lcov |
| `bench` | Optimized build for benchmarks |

```bash
cmake --preset=asan && cmake --build --preset=asan && ctest --preset=asan
cmake --preset=bench && cmake --build --preset=bench
./build-bench/benchmark/kalshi_bench --benchmark_time_unit=us
cmake --build build -t format-fix     # auto-fix formatting
```

Testing layers (unit / replay / demo-conformance / capture):
[docs/testing.md](docs/testing.md). Dev environment details:
[docs/dev-setup.md](docs/dev-setup.md). Deployment:
[docs/AWS_SETUP.md](docs/AWS_SETUP.md) and [deploy/](deploy/README.md).

### Project structure

```
kalshi-market-maker/
├── source/            # library + kalshi_mm entry point (main.cpp, app_modes, cli)
├── test/
│   ├── source/        # unit tests (one file per component, fakes for all I/O)
│   ├── integration/   # replay_session_test (hermetic) + demo_conformance_test (live demo)
│   ├── replay/        # replay tests against recorded fixtures
│   ├── fuzz/          # libFuzzer targets
│   └── fixtures/      # recorded WS + REST message sequences
├── benchmark/         # Google Benchmark microbenchmarks
├── scripts/           # run_demo.sh, analysis scripts, pre-commit hook
├── docs/              # architecture, guards, API reference, ADRs, papers
└── deploy/            # systemd units + VM setup
```

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs four jobs on every push and PR:

| Job | What it checks |
|---|---|
| build-and-test | `cmake --preset dev`, all unit + integration tests |
| asan | Same tests under AddressSanitizer |
| coverage | Line coverage via lcov + llvm-cov, uploaded as artifact |
| benchmark | Builds and runs the benchmark binary |
