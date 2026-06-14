# Claude Instructions — Kalshi Market Maker

## Test-Driven Development

All code in this project follows strict TDD. The workflow for every new component is:

1. **Write the test first.** Define the interface by writing tests against it before any implementation exists. Tests should fail to compile or fail at runtime initially.
2. **Write the minimal implementation** to make the tests pass. No production code is written without a failing test that demands it.
3. **Refactor.** Clean up implementation and tests without changing behavior. All tests must still pass.

Concretely:
- Every new `.hpp` or `.cpp` in `source/` must have a corresponding test file in `test/source/`.
- Mock external dependencies (HTTP, WebSocket, exchange) with interfaces + fakes so unit tests are fast and hermetic.
- Integration tests live in a separate `test/integration/` directory and are gated behind a CMake option.
- Never add a function to a header without first writing a test that calls it.

## Code Quality

A pre-commit hook (`scripts/pre-commit`) runs automatically on every `git commit`:
- **clang-format** — checks all staged `.cpp`/`.hpp` files against `.clang-format` (LLVM style). Fix with `cmake --build build -t format-fix`.
- **clang-tidy** — checks staged `.cpp` files against `.clang-tidy` using `build/compile_commands.json`. Requires `cmake --preset=dev` to have been run first.

On a fresh clone, install the hook once: `bash scripts/install-hooks.sh`

## Commits

- Commit small and often — after each test passes, after each phase completes, after any clean refactor.
- Message format: `feat/fix/refactor: <description>`, 15 words or fewer.
- No co-author lines, no Claude attribution of any kind.

Examples:
```
feat: add types.hpp with Order, Fill, Market, Orderbook structs
feat: add auth RSA-SHA256 signing with OpenSSL
fix: clamp complement_price to valid range
refactor: extract kalshi_add_test helper in test CMakeLists
```

## Architecture Diagrams

Every phase of the build plan has a Mermaid diagram. When adding a new component:
- Update `PLAN.md` with a Mermaid diagram showing where the new component fits in the overall system.
- Diagrams use `graph TD` (top-down) for system architecture and `sequenceDiagram` for request/response flows.
- Keep diagrams focused: each one should show only the components relevant to that phase, with already-built components shown as grey/completed nodes using `style NodeName fill:#555`.
