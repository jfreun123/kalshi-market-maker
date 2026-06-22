# Testing

## Layers

```
Manual / Live Testing        ← prod/demo account
Integration Tests            ← demo API, real network (KALSHI_INTEGRATION_TESTS=ON)
Contract / Snapshot Tests    ← recorded real API responses in test/fixtures/
Unit Tests                   ← fakes, no network (default, run by pre-commit hook)
Sanitizers (ASAN / TSAN)     ← run on unit + integration tests
```

## Unit Tests

```bash
cmake --preset=dev && cmake --build --preset=dev && ctest --preset=dev
```

All external I/O replaced by fakes (`FakeTransport`, `FakeWebSocket`). Fast, hermetic, deterministic.

## Sanitizers

```bash
cmake --preset=asan && cmake --build --preset=asan && ctest --preset=asan  # memory
cmake --preset=tsan && cmake --build --preset=tsan && ctest --preset=tsan  # races
```

ASAN and TSAN cannot run simultaneously. Run before any significant merge.

## Contract / Snapshot Tests

Capture real API responses into `test/fixtures/`, then assert our parsers handle them. Catches API schema drift without a live connection.

```bash
curl -s -H "..." https://demo-api.kalshi.co/trade-api/v2/markets/TICKER/orderbook \
  > test/fixtures/orderbook_TICKER.json
```

## Integration Tests

Gated behind `-DKALSHI_INTEGRATION_TESTS=ON`. Hit the Kalshi demo environment — same API contract as prod, fake money.

```bash
cmake --preset=dev -DKALSHI_INTEGRATION_TESTS=ON \
  -DKALSHI_API_KEY=$KALSHI_API_KEY \
  -DKALSHI_PRIVATE_KEY_PATH=$KALSHI_PRIVATE_KEY_PATH \
  -DKALSHI_BASE_URL=https://demo-api.kalshi.co/trade-api/v2
cmake --build --preset=dev
ctest --preset=dev -R Integration
```

## Replay Testing

Record WebSocket deltas to a `.jsonl` file (see `test/fixtures/session_synthetic.jsonl` for format), then drive the same components offline via `FakeWebSocket::enqueue_message`. Used to tune `QuoterConfig` parameters without risking capital.
