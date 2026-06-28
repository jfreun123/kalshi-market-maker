# kalshi-market-maker

A C++23 automated market maker for [Kalshi](https://kalshi.com) prediction markets. Quotes two-sided markets across configurable tickers, manages risk via a constraint bitset, and detects adverse selection with a rolling-window fill-rate guard.

## Architecture

```
WebSocketClient ──► Quoter ──► OrderManager ──► RestClient ──► Kalshi API
     │                │              │
     │            FairValueEngine    └── RiskManager
     │           (IPricingModel)         (Constraint bitset)
     │
     └── AdverseSelectionGuard
```

**Key components:**

| Component | File | Responsibility |
|---|---|---|
| `RestClient` | `rest_client.hpp` | Place/cancel orders, poll fills (HTTP) |
| `WebSocketClient` | `websocket_client.hpp` | Orderbook snapshots + fill events (WS) |
| `OrderManager` | `order_manager.hpp` | Track open orders, positions, realized PnL |
| `RiskManager` | `risk_manager.hpp` | 8-bit constraint bitset; blocks orders when any bit is set |
| `FairValueEngine` | `fair_value.hpp` | Delegates to a pluggable `IPricingModel` |
| `HeuristicModel` | `pricing_model.hpp` | Mid-price + time-decay + inventory skew |
| `TheoGrid` | `theo_grid.hpp` | Bilinear interpolation table for fast repricing |
| `Quoter` | `quoter.hpp` | Computes bid/ask, reprices on orderbook delta |
| `AdverseSelectionGuard` | `adverse_selection.hpp` | Pulls quotes when fill rate exceeds threshold |
| `Auth` | `auth.hpp` | RSA-SHA256 request signing |

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
    "quote_size": 10
  },
  "risk": {
    "max_position_per_market": 100,
    "max_open_orders_per_market": 4,
    "max_order_size": 25,
    "daily_loss_limit": -500.0
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

# Demo / UAT environment
# Set base_url and ws_url to the demo endpoints in config.json:
#   "base_url": "https://demo-api.kalshi.co/trade-api/v2"
#   "ws_url":   "wss://demo-api.kalshi.co/trade-api/ws/v2"
```

### Workflow: scan then trade

Run `--scan` first to find liquid markets, then copy the top tickers into `target_tickers` in your config and start the market maker:

```bash
./build/source/kalshi_mm --scan config.json 2>&1 | grep "ticker="
# Pick tickers from the output, add them to config.json → target_tickers
./build/source/kalshi_mm config.json
```

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
│   ├── auth.hpp/cpp         # RSA-SHA256 signing
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
