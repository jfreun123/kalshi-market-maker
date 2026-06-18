# ADR-002: Timestamp Parsing

## Status: Accepted

## Context

Kalshi REST responses include ISO-8601 timestamps (e.g. `"2024-03-15T14:30:00Z"`).
These must be parsed into `std::chrono::system_clock::time_point` for use in
`Order` and `Fill` structs. Options:

- **`std::chrono::parse`** (C++20) — not widely available; libc++ support
  incomplete as of the project start date.
- **`strptime` + `timegm`** — POSIX, available on every Linux/macOS target,
  zero dependencies.
- **Howard Hinnant's date library** — correct and portable, but an extra
  dependency with non-trivial integration.

## Decision

Use **`strptime` + `timegm`** in an internal helper (`parse_iso8601`).

Reasons:
1. Both are available on every Linux target (glibc) without additional
   dependencies.
2. The format is fixed (`%Y-%m-%dT%H:%M:%SZ`) — no locale or format
   negotiation needed.
3. `timegm` correctly treats the broken-down time as UTC, unlike `mktime`
   which uses the local timezone.

## Consequences

- `strptime`/`timegm` are not part of the C++ standard. A future port to a
  non-POSIX target would need to replace `parse_iso8601`.
- `WARN_IF_UNDOCUMENTED` is disabled for this helper since it lives in an
  anonymous namespace and is not part of the public API.
