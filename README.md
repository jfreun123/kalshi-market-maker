# kalshi-market-maker

A C++23 automated market maker for [Kalshi](https://kalshi.com) prediction markets. Quotes two-sided markets across configurable tickers, manages risk via a constraint bitset, and detects adverse selection with a rolling-window fill-rate guard.

## Architecture

```
WebSocketClient ──► TradingSession ──► Quoter ──► OrderManager ──► RestClient ──► Kalshi API
                          │                │            │   │
                          │            FairValueEngine   │   └── RiskManager (Constraint bitset
                          │           (IPricingModel)     │        + global kill-switch)
                          ├── AdverseSelectionGuard       └── Portfolio (read-model) ──► RiskManager
                          └── (--capture tees raw WS + REST via Capturing* decorators)
```

**Key components:**

| Component | File | Responsibility |
|---|---|---|
| `RestClient` | `rest_client.hpp` | Place/cancel orders, poll fills (V2 HTTP) |
| `WebSocketClient` | `websocket_client.hpp` | Orderbook snapshots + fill events (WS) |
| `TradingSession` | `trading_session.hpp` | Owns the domain reactions (snapshot/delta/fill, portfolio kill-switch, status); the seam shared by prod, tests, and replay |
| `OrderManager` | `order_manager.hpp` | Track open orders, positions, realized + unrealized PnL |
| `RiskManager` | `risk_manager.hpp` | Constraint bitset; blocks orders when any bit is set; `update_portfolio` is the global kill-switch |
| `Portfolio` | `portfolio.hpp` | Read-model: total PnL, capital-at-risk, per-event risk; `reconcile()` vs exchange |
| `FairValueEngine` | `fair_value.hpp` | Delegates to a pluggable `IPricingModel` |
| `HeuristicModel` | `pricing_model.hpp` | Mid-price + time-decay + inventory skew (default) |
| `ViewBasedModel` | `pricing_model.hpp` | Favorite-longshot debiasing `π*=(P−β/2)/(1−β)`; opt-in via `use_view_based_pricing` |
| `TheoGrid` | `theo_grid.hpp` | Bilinear interpolation table for fast repricing |
| `Quoter` | `quoter.hpp` | Computes bid/ask, reprices on orderbook delta |
| `AdverseSelectionGuard` | `adverse_selection.hpp` | Pulls quotes when fill rate exceeds threshold |
| `FlowImbalanceGuard` | `flow_imbalance.hpp` | Tracks one-sided fill flow; Quoter widens spread when imbalanced (Palumbo E_win signal) |
| `TickerScanner` | `ticker_scanner.hpp` | Ranks markets by volume/spread; writes a trade config |
| `Capturing{WebSocket,HttpTransport}` | `capture.hpp` | Tee raw WS frames / REST responses for `--capture` replay |
| `Auth` | `auth.hpp` | RSA-PSS-SHA256 request signing |

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
# Install the pre-commit hook (once per clone)
bash scripts/install-hooks.sh

# Configure + build (Debug, static analysis enabled)
cmake --preset=dev
cmake --build --preset=dev

# Run all tests
ctest --preset=dev
```

## Configuration

Copy the example config and fill in your credentials:

```bash
cp config.example.json config.json
```

```json
{
  "api_key": "YOUR_KALSHI_API_KEY",
  "private_key_path": "/path/to/kalshi-private-key.pem",
  "base_url": "https://api.elections.kalshi.com/trade-api/v2",
  "ws_url": "wss://api.elections.kalshi.com/trade-api/ws/v2",
  "target_tickers": ["KXBTCD-25DEC31-B90000"],
  "quoter": {
    "target_spread_cents": 4,
    "skew_per_contract_cents": 0.05,
    "reprice_threshold_cents": 1,
    "quote_size": 10,
    "imbalance_spread_cents": 2,
    "min_spread_cents": 3,
    "use_view_based_pricing": false,
    "view_debias_beta": 0.09,
    "maker_fee_rate": 0.0
  },
  "flow": {
    "window_seconds": 300,
    "imbalance_ratio_threshold": 2.0,
    "min_flow_volume": 20
  },
  "risk": {
    "max_position_per_market": 100,
    "max_open_orders_per_market": 4,
    "max_order_size": 25,
    "daily_loss_limit": -500.0,
    "max_total_exposure_dollars": 10000.0,
    "max_total_loss_dollars": -1000.0,
    "min_quote_price_cents": 10,
    "max_quote_price_cents": 90,
    "max_drawdown_dollars": 500.0
  }
}
```

Generate an RSA key pair for API authentication:

```bash
openssl genrsa -out kalshi-private-key.pem 2048
openssl rsa -in kalshi-private-key.pem -pubout -out kalshi-public-key.pem
# Upload the public key at kalshi.com → Settings → API Keys
```

## Running

```bash
# Scan active markets and print ranked candidates (no orders placed)
./build/source/kalshi_mm --scan config.json

# Live trading (target_tickers must be set in config.json)
./build/source/kalshi_mm config.json

# Paper trading (simulates fills locally, no real orders)
./build/source/kalshi_mm --paper config.json

# Reconcile local accounting against the exchange (no orders placed).
# Exits 0 if in sync, non-zero on any position mismatch — good for CI/pre-trade.
./build/source/kalshi_mm --reconcile config.json

# Capture a live session for replay (no orders placed). Records raw inbound WS
# frames to <dir>/session.jsonl (replay-compatible) and seed REST responses to
# <dir>/rest.jsonl. Runs until Ctrl-C. Point config at demo to capture a UAT run:
./build/source/kalshi_mm --capture capture/demo-run config-demo.json
# Replay it through the full stack: drop session.jsonl into test/fixtures/ and
# point the integration test at it (see test/integration/replay_session_test.cpp).

# Demo / UAT environment
# Set base_url and ws_url to the demo endpoints in config.json:
#   "base_url": "https://demo-api.kalshi.co/trade-api/v2"
#   "ws_url":   "wss://demo-api.kalshi.co/trade-api/ws/v2"
```

### Workflow: scan then trade

`--scan` is fully automated: it finds liquid markets and writes a ready-to-run
trade config with the top tickers already filled in. The daily flow is two commands:

```bash
./build/source/kalshi_mm --scan config.json   # → writes config.trade.json
./build/source/kalshi_mm config.trade.json    # → makes markets on the top-N
```

The generated `config.trade.json` is a verbatim copy of the base config (same
credentials, quoter, risk, and scanner sections) with **`target_tickers` replaced**
by the top `trade_top_n` results. The output name is derived from the base config:
`config.json` → `config.trade.json`, `config.demo.json` → `config.demo.trade.json`.
It is gitignored (it contains the copied credentials).

The scanner pages through all **open** markets (`status=open`, 1000/page) and ranks
them by a volume-weighted score with a spread-quality term. Filters are set in the
optional `scanner` section of the config:

```json
"scanner": {
  "min_price_cents": 15,
  "max_price_cents": 85,
  "min_spread_cents": 3,
  "max_spread_cents": 10,
  "min_volume_24h": 1000.0,
  "min_days_to_close": 1.0,
  "max_days_to_close": 90.0,
  "trade_top_n": 5
}
```

All fields are optional and fall back to the defaults shown above. `min_volume_24h`
filters on **24-hour** contract volume (live flow), which is also the basis for the
ranking score. `trade_top_n` controls how many top tickers go into the generated
trade config (default 5 — safe for the Basic rate-limit tier). Loosen the volume,
price, and spread ranges for demo (where liquidity is thin); tighten them for
production. Switching environments is just a matter of changing `base_url`/`ws_url`
and these thresholds.

Each scan also writes **`scan_results.json`** — a ranked JSON document with a
top-level `tickers` array (rank order) and a `markets` array with full per-market
detail, for any custom downstream tooling.

### Portfolio risk & reconciliation

`OrderManager` is the single source of truth for positions and PnL; `Portfolio`
(`portfolio.hpp`) is a pure read-model over it that aggregates **total realized +
unrealized (mark-to-market) PnL**, **total capital at risk**, and a per-**event**
breakdown (correlated strikes rolled up). It's logged each status interval.

Four portfolio-level safety checks complement the per-market risk limits. The
first three form a **global kill-switch**: `RiskManager::update_portfolio()`
consumes the read-model snapshot (rebuilt ~1s, sub-ms at this scale) and any
tripped bit halts **all** quoters at once.

- **Over-exposure** — `risk.max_total_exposure_dollars` caps total capital at risk
  across all markets. Per-market limits don't bound aggregate exposure; this does.
  A breach trips `kOverExposure` and halts all quoting.
- **Total-loss floor** — `risk.max_total_loss_dollars` caps total PnL including
  **unrealized** mark-to-market, anchored at break-even. The realized-only
  `daily_loss_limit` can't see a book bleeding while holding inventory; a breach
  trips `kPortfolioLoss`.
- **Drawdown** — `risk.max_drawdown_dollars` caps how much total PnL may give back
  from its **session high-water mark**. Unlike the loss floor, it protects gains —
  it can fire while still net profitable (e.g. peak +$700 → +$100 is a $600
  drawdown). A breach trips `kDrawdown`; `resume()` re-anchors the peak.

  All three require a manual `resume()` to clear (no auto-resume into a crashing
  market) and route through the same flatten-on-halt path.
- **Reconciliation** — local accounting is rebuilt from a WebSocket fill stream,
  which can drift from the exchange (missed messages, reconnects). The bot fetches
  Kalshi's authoritative `GET /portfolio/positions` every ~2 min during live
  trading and compares net positions per ticker. Any mismatch trips
  `kModelDiverge` and halts quoting — if you don't know your true position, you
  stop. Run `--reconcile` for a one-shot check (exits non-zero on mismatch).

## Development

### CMake presets

| Preset | Purpose |
|---|---|
| `dev` | Debug build with clang-tidy + cppcheck |
| `asan` | AddressSanitizer |
| `tsan` | ThreadSanitizer |
| `coverage` | Line coverage via lcov |
| `bench` | Optimized build for benchmarks |

```bash
# AddressSanitizer
cmake --preset=asan && cmake --build --preset=asan && ctest --preset=asan

# Coverage report
cmake --preset=coverage && cmake --build --preset=coverage && ctest --preset=coverage

# Benchmarks
cmake --preset=bench && cmake --build --preset=bench
./build-bench/benchmark/kalshi_bench --benchmark_time_unit=us
```

### Code quality

```bash
# Auto-fix clang-format
cmake --build build -t format-fix

# Check formatting (what CI runs)
cmake --build build -t format-check
```

The pre-commit hook runs clang-format and clang-tidy automatically on every `git commit`. Install it once with `bash scripts/install-hooks.sh`.

### Project structure

```
kalshi-market-maker/
├── source/                  # Library + main entry point
│   ├── types.hpp            # Order, Fill, Market, Orderbook structs
│   ├── auth.hpp/cpp         # RSA-PSS-SHA256 signing
│   ├── rest_client.hpp/cpp  # HTTP REST API client
│   ├── websocket_client.hpp/cpp  # WS orderbook + fill feed
│   ├── order_manager.hpp/cpp     # Order lifecycle + PnL tracking
│   ├── risk_manager.hpp/cpp      # Constraint bitset, position limits
│   ├── fair_value.hpp/cpp        # FairValueEngine (strategy pattern)
│   ├── pricing_model.hpp/cpp     # IPricingModel + HeuristicModel
│   ├── theo_grid.hpp/cpp         # Bilinear interpolation grid
│   ├── quoter.hpp/cpp            # Two-sided quoting logic
│   ├── adverse_selection.hpp/cpp # Rolling-window fill-rate guard
│   ├── logger.hpp/cpp            # spdlog wrapper (get/set_logger)
│   ├── config.hpp/cpp            # JSON config loader
│   ├── paper_transport.hpp/cpp   # Simulated REST for paper trading
│   └── main.cpp
├── test/
│   ├── source/              # Unit tests (one file per component)
│   ├── replay/              # Replay tests against recorded fixtures
│   ├── fuzz/                # libFuzzer fuzz targets
│   └── fixtures/            # Recorded WS + REST message sequences
├── benchmark/               # Google Benchmark microbenchmarks
├── cmake/                   # FetchContent modules for dependencies
├── scripts/                 # pre-commit hook + installer
├── config.example.json
├── CMakeLists.txt
└── CMakePresets.json
```

## CI

GitHub Actions runs four jobs on every push to `main`:

| Job | What it checks |
|---|---|
| Build & Test | `cmake --preset dev`, all unit + replay tests |
| AddressSanitizer | Same tests under ASAN |
| Coverage | Line coverage via lcov + llvm-cov; report uploaded as artifact |
| Benchmark | Builds and runs the benchmark binary |
