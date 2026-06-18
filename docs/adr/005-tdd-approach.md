# ADR-005: Test-Driven Development

## Status: Accepted

## Context

Market making software handles real money. Bugs in order placement, risk
limiting, or fill accounting have direct financial consequences. The team
considered:

- **Test-after**: write production code first, add tests for coverage.
- **Test-driven (TDD)**: write a failing test first, then minimal production
  code to pass it.
- **No tests**: move fast, debug in production.

## Decision

Strict **TDD** for all production code in `source/`.

Rules enforced:
1. Every `.hpp`/`.cpp` in `source/` has a corresponding test file in
   `test/source/`.
2. No function is added to a header without a failing test that calls it.
3. External dependencies (HTTP, WebSocket, exchange) are hidden behind
   interfaces with hermetic fakes for unit tests.
4. Integration tests and replay tests are gated behind CMake options so the
   fast unit-test loop stays under one second.

## Consequences

- Writing tests first forces interface design before implementation, resulting
  in smaller, more composable units.
- The fake/interface pattern adds a layer of indirection. The payoff is that
  every component can be tested without a network connection.
- Discipline is required: the pre-commit hook enforces clang-tidy and
  clang-format, but cannot enforce "test first." That remains a team norm.
- The test suite (162+ tests as of Phase 19) serves as living documentation
  of every component's expected behavior.
