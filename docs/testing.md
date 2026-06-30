# Testing

## Layers

```
Manual / Live Testing        ← prod/demo account; `--capture` records a real session
Replay Integration Tests     ← captured/synthetic session → full TradingSession stack
                               (hermetic, KALSHI_INTEGRATION_TESTS, default ON)
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

Record real API responses into `test/fixtures/`, then assert our parsers handle them. Catches API schema drift without a live connection. The repeatable way to record both WS frames and REST responses is `--capture` (below); a one-off REST body can also be grabbed with `curl`.

## Integration Tests (full-stack replay)

`test/integration/replay_session_test.cpp` replays a captured/synthetic session through the **real production wiring** — `WebSocketClient` parser → `TradingSession` → Quoter / OrderManager (over `PaperTransport`) / RiskManager / Portfolio — and asserts end-to-end invariants (valid BBO, fills applied, quotes placed, risk not spuriously halted, portfolio computable). It is **hermetic** (checked-in fixtures, no network) and **gated** behind `KALSHI_INTEGRATION_TESTS` (default **ON**); a future networked test would get its own off-by-default option.

```bash
cmake --preset=dev && cmake --build --preset=dev
ctest --preset=dev -R ReplaySession
# disable: cmake --preset=dev -DKALSHI_INTEGRATION_TESTS=OFF
```

This is what validates real demo field shapes: a parser/schema mismatch surfaces here as a broken orderbook or a missing fill. It already caught the broken paper-mode V2 order schema.

## Capturing a real session for replay

`--capture <dir>` connects to the configured environment and records the session **without placing orders**: raw inbound WS frames → `<dir>/session.jsonl` (one per line, replay-compatible), and seed REST responses → `<dir>/rest.jsonl`.

```bash
./build/source/kalshi_mm --capture capture/demo-run config-demo.json   # Ctrl-C to stop
```

Drop the resulting `session.jsonl` into `test/fixtures/` and drive it through `FakeWebSocket::enqueue_message` in a replay test. Note the existing `ReplaySessionTest` asserts are tied to the synthetic fixture (ticker `REPLAY-TICK`), so a real capture needs its own assertions. Also useful for tuning `QuoterConfig` offline without risking capital.
