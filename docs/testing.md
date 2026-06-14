# Testing Strategy

## Layers

```
┌─────────────────────────────────────────┐
│          Manual / Live Testing          │  ← prod/demo account, real money
├─────────────────────────────────────────┤
│         Integration Tests               │  ← demo API, real network
├─────────────────────────────────────────┤
│      Contract / Snapshot Tests          │  ← recorded real API responses
├─────────────────────────────────────────┤
│           Unit Tests                    │  ← fakes, no network (default)
├─────────────────────────────────────────┤
│     Sanitizers (ASAN / TSAN)            │  ← run on unit + integration tests
└─────────────────────────────────────────┘
```

---

## 1. Unit Tests (default)

**What:** Every component tested in isolation. External dependencies (HTTP, WebSocket, exchange) are replaced by fakes injected via interfaces (`IHttpTransport`, `IWebSocket`).

**Run:**
```bash
cmake --preset=dev && cmake --build --preset=dev && ctest --preset=dev
```

**Philosophy:** Fast (sub-second), hermetic (no network), deterministic. The pre-commit hook runs these automatically. A new component is not considered done until its unit tests pass with zero clang-tidy warnings.

**Current coverage:**
| Component | Test file | Tests |
|---|---|---|
| Types | `test/source/types_test.cpp` | 19 |
| Auth | `test/source/auth_test.cpp` | 7 |

---

## 2. Sanitizer Runs

**What:** The same unit (and integration) tests compiled with Clang sanitizers. Catch bugs that pass functional tests.

**AddressSanitizer** — heap/stack overflows, use-after-free, double-free:
```bash
cmake --preset=asan && cmake --build --preset=asan && ctest --preset=asan
```

**ThreadSanitizer** — data races between the WebSocket callback thread and the main loop:
```bash
cmake --preset=tsan && cmake --build --preset=tsan && ctest --preset=tsan
```

Run sanitizers before every significant merge. ASAN and TSAN cannot run simultaneously (conflicting instrumentation).

---

## 3. Contract / Snapshot Tests

**What:** Record real API responses from the demo environment (JSON blobs checked into `test/fixtures/`), then assert our parsers handle them correctly. Catches API contract drift without needing a live connection during CI.

**How to add a fixture:**
```bash
# Capture a real response
curl -s -H "..." https://demo-api.kalshi.co/trade-api/v2/markets/KXBTCD-25DEC31-T50000/orderbook \
  > test/fixtures/orderbook_KXBTCD.json

# Write a test that reads and parses it
TEST(ContractTest, OrderbookParses) {
    auto json = read_file("test/fixtures/orderbook_KXBTCD.json");
    auto book = parse_orderbook(json);
    EXPECT_FALSE(book.ticker.empty());
    // ...
}
```

**When to update fixtures:** When Kalshi changes their API schema. Check `test/fixtures/` into git so CI can run without network access.

---

## 4. Integration Tests

**What:** Tests that exercise real HTTP and WebSocket code against Kalshi's **demo environment**. Gated behind a CMake option so they never run in CI without explicit opt-in.

### Kalshi Demo Environment

Kalshi operates a full paper-trading sandbox at `demo.kalshi.co`:
- Same API contract and endpoints as production
- Same RSA auth mechanism
- Fake money — no financial risk
- Markets mirror production but settlement is simulated
- API keys are separate from prod — create an account at `demo.kalshi.co` and generate keys there

**Enable integration tests:**
```bash
cmake --preset=dev -DKALSHI_INTEGRATION_TESTS=ON \
  -DKALSHI_API_KEY=$KALSHI_API_KEY \
  -DKALSHI_PRIVATE_KEY_PATH=$KALSHI_PRIVATE_KEY_PATH \
  -DKALSHI_BASE_URL=https://demo-api.kalshi.co/trade-api/v2
cmake --build --preset=dev
ctest --preset=dev -R Integration
```

Integration tests live in `test/integration/` and are excluded from the default `ctest` run. They will be added in Phase 3 (RestClient) and Phase 5 (WebSocketClient).

---

## 5. Docker-Based Testing

**What:** Run integration tests inside a container with all dependencies pre-installed. Useful for:
- Ensuring a clean-room build (no local tool version drift)
- Running integration tests in CI without installing dependencies on the runner
- Isolating the process's network access to only what it needs

**Planned `Dockerfile.test`** (to be added):
```dockerfile
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y \
    cmake ninja-build clang-18 clang-tidy clang-format-18 \
    libssl-dev cppcheck
COPY . /app
WORKDIR /app
RUN cmake --preset=dev && cmake --build --preset=dev
CMD ["ctest", "--preset=dev", "--output-on-failure"]
```

**Run:**
```bash
docker build -f Dockerfile.test -t kalshi-mm-test .
docker run --rm \
  -e KALSHI_API_KEY=$KALSHI_API_KEY \
  -e KALSHI_PRIVATE_KEY_PATH=/run/secrets/kalshi_key \
  --mount type=secret,id=kalshi_key \
  kalshi-mm-test
```

Docker is the recommended way to run integration tests in CI (GitHub Actions, etc.) since it avoids leaking credentials into runner environment logs.

---

## 6. Backtesting / Replay (Future)

The fair value engine and quoter are designed to be pure functions of their inputs. This means they can be driven by recorded market data offline — no exchange connection required.

**Planned approach:**
1. Record WebSocket orderbook deltas and fills to a log file during a live session
2. Write a `ReplayDriver` that reads the log and drives the same components as the live main loop
3. Measure realized spread, inventory excursions, and PnL against the recorded fills

This is the most effective way to tune `QuoterConfig` parameters (spread, skew, reprice threshold) without risking capital.

---

## What to Test at Each Phase

| Phase | Unit tests | Integration tests | Contract tests |
|---|---|---|---|
| 1 Types | Struct construction, helpers | — | — |
| 2 Auth | Signature verification, header structure | Sign a real request | — |
| 3 REST Client | FakeTransport responses | `GET /markets`, `GET /orderbook` | Orderbook JSON fixture |
| 4 Local Orderbook | Delta sequences, BBO calc | — | — |
| 5 WebSocket | FakeWebSocket message dispatch | Subscribe + receive delta | Delta message fixture |
| 6 Order Manager | State machine, fill accumulation | Place + cancel on demo | Order JSON fixture |
| 7 Risk Manager | Limit enforcement, kill switch | — | — |
| 8 Fair Value | Model outputs, skew calculation | — | — |
| 9 Quoter | Quote generation, reprice logic | Full quote loop on demo | — |
| 10 Main Loop | — | End-to-end on demo | — |
