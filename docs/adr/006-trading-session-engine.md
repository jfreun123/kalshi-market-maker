# ADR-006: TradingSession Engine

## Status: Accepted (2026-06-29)

## Context

The market-making loop's domain reactions — handling orderbook snapshots,
deltas, and fills; driving the quoter; running the portfolio kill-switch;
logging status — originally lived as free functions and inline lambdas inside
`main.cpp`'s `main()`. This had two costs:

- **Tests duplicated the wiring.** `main_loop_test` re-created the WS→quoter→risk
  wiring by hand, so tests exercised a *copy* of the production flow, not the
  flow itself. The copy could (and did) drift.
- **Replay was impossible.** To replay a captured exchange session through the
  real components, that wiring had to be reachable without launching the full
  process (logger, signals, sockets, file persistence).

## Decision

Extract the domain reactions into a `TradingSession` class
(`source/trading_session.hpp/.cpp`) that owns the live orderbook map and holds
references to `OrderManager`, `RiskManager`, and `Quoter`.

- `main.cpp` is reduced to process/IO setup: config, logger, signals,
  transports, the WS thread, and a `persist_pnl` fill listener. It constructs a
  `TradingSession` and forwards WS callbacks to its `on_snapshot` / `on_delta` /
  `on_fill` / `on_disconnect` methods.
- Periodic work is exposed as `run_portfolio_risk()` (the ~1s kill-switch) and
  `log_status()`.
- The session is free of IO concerns and uses the `get_logger()` abstraction, so
  it is constructed directly in unit tests and in the replay integration test.

## Consequences

- The integration test (`test/integration/replay_session_test.cpp`) drives the
  **real** wiring, not a copy. This immediately caught a latent bug: paper mode
  placed zero orders because `PaperTransport` still spoke the pre-V2 order schema
  (see `006`'s sibling fix in `paper_transport.cpp`).
- One seam is shared by production, unit tests, and `--capture` replay, removing
  the `main_loop_test` duplication risk.
- `main()`'s cognitive complexity dropped back under the clang-tidy threshold.
- Trade-off: `TradingSession` holds references to objects `main()` owns; their
  lifetimes must outlive the session. This is enforced by construction order in
  `main()` and in the test fixtures, not by the type system.
