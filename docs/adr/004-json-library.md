# ADR-004: JSON Library

## Status: Accepted

## Context

JSON is used throughout: REST request bodies, REST response parsing, WebSocket
message dispatch, and config file loading. Options:

- **nlohmann/json** — header-only, expressive C++ API, widely used.
- **RapidJSON** — fastest DOM parser, verbose SAX API, no single-header option.
- **simdjson** — highest throughput (SIMD), read-only parsed view, less
  ergonomic for construction.
- **Boost.JSON** — requires Boost.

## Decision

Use **nlohmann/json** via CMake FetchContent.

Reasons:
1. Header-only — same vendoring story as cpp-httplib.
2. Expressive subscript + `.get<T>()` API keeps parsing code readable.
3. JSON construction for request bodies is first-class, unlike simdjson.
4. Parse throughput is not a bottleneck: messages arrive at a few hundred
   per second, not millions.

## Consequences

- nlohmann/json has higher constant-factor overhead than RapidJSON/simdjson.
  If profiling ever shows JSON parsing on the critical path, swap the
  implementation behind the existing `RestClient`/`WebSocketClient` parsing
  helpers without changing callers.
