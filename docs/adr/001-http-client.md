# ADR-001: HTTP Client Library

## Status: Accepted

## Context

The market maker needs to make authenticated HTTPS REST calls to Kalshi's API
(place orders, cancel orders, query orderbook). Choices evaluated:

- **libcurl** — ubiquitous, mature, C API with complex lifetime management
- **cpp-httplib** — header-only C++11 library with built-in OpenSSL/TLS support
- **Boost.Beast** — full HTTP/WebSocket stack, heavy dependency

## Decision

Use **cpp-httplib** (`yhirose/cpp-httplib`) via CMake FetchContent.

Reasons:
1. Header-only — zero link-time cost, trivial to vendor or fetch.
2. Native HTTPS via OpenSSL, which is already a required dependency for RSA
   auth signing. No additional TLS library needed.
3. Simple synchronous API matches the use case (low-frequency order management,
   not streaming).
4. The `IHttpTransport` interface abstracts cpp-httplib behind a virtual
   boundary, making it replaceable in tests and paper-trading mode without
   touching call sites.

## Consequences

- Synchronous HTTP means each REST call blocks the calling thread. Acceptable
  because order placement is infrequent and happens on a dedicated thread.
- If async HTTP is ever needed, swap the concrete `HttpTransport` without
  changing the rest of the codebase.
