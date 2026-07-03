# ADR-003: WebSocket Library

## Status: Accepted

## Context

The market maker subscribes to Kalshi's WebSocket feed for real-time orderbook
snapshots, deltas, and fill notifications. Candidates:

- **Boost.Beast** — standard-quality, but requires Boost and async I/O setup.
- **libwebsockets** — C library, complex API for a C++ project.
- **IXWebSocket** (`machinezone/IXWebSocket`) — C++14 header+source library,
  TLS via OpenSSL, automatic reconnect.
- **cpp-httplib WebSocket** — not supported (HTTP only).

## Decision

Use **IXWebSocket** via CMake FetchContent.

Reasons:
1. Reuses OpenSSL already required for auth — no new TLS dependency.
2. Built-in reconnect logic handles exchange disconnects without custom retry
   code.
3. Minimal API surface: `setUrl`, `setOnMessageCallback`, `start`/`stop`.
4. `IWebSocket` interface wraps IXWebSocket behind a virtual boundary so unit
   tests use `FakeWebSocket` without any network.

## Consequences

- IXWebSocket runs callbacks on an internal thread; shared state touched in
  callbacks must be thread-safe (protected by mutex or atomic).
- The `FakeWebSocket` test double is synchronous (no threading), so replay
  tests are deterministic and fast.
- Superseding reason 2: IXWebSocket's built-in auto-reconnect is disabled in
  `WebSocketClient` because it would replay stale auth headers on reconnect.
  Reconnects are driven by the client's own exponential backoff (base delay
  doubling per consecutive failure, capped at `kMaxReconnectDelay` = 60s,
  reset on a successful connect).
