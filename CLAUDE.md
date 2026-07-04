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

**clang-tidy is probably right.** When clang-tidy warns about something, the default assumption is to fix the code, not suppress the warning. Common cases and what to do:

- `readability-identifier-length` — single-letter or two-letter names (`o`, `f`, `ob`) are not acceptable. Use descriptive names (`order`, `fill`, `book`) everywhere, including in tests. Tests are documentation.
- `readability-magic-numbers` / `cppcoreguidelines-avoid-magic-numbers` — numeric literals scattered through code have no meaning to a reader. Extract them to named `constexpr` constants that explain intent (e.g., `constexpr int kYesBidPrice = 52`).
- `readability-uppercase-literal-suffix` — write `1U`, `1L`, `1UL`, not `1u`, `1l`, `1ul`. The lowercase forms are visually ambiguous.

Only suppress a clang-tidy warning (`// NOLINT(check-name)`) when you have a specific, documented reason it doesn't apply. "It's test code" is not a reason.

## Comments

**No comments, with one exception: a single header-doc comment at the very top of a `.hpp` file** describing what that header provides. Nowhere else — no inline comments, no comments above functions/classes/members, no explanatory comments in `.cpp` files, no comments in tests. Code must read clearly on its own: descriptive names, small functions, named `constexpr` constants instead of a comment explaining a literal. If code seems to need a comment to be understood, restructure or rename until it doesn't.

`// NOLINT(...)` suppressions and `#pragma` directives are not comments in this sense and remain allowed where justified.

## Secrets — Never Commit

**NEVER write secrets to git or GitHub under any circumstances.** This includes:
- API keys, key IDs, tokens, passwords
- RSA private keys (`.pem` files or inline PEM strings)
- `config.json` (contains live credentials)
- `pnl_state.json` (contains financial state)

These are covered by `.gitignore`. If you are ever unsure whether a file contains a secret, do not add it to a commit. If a secret is accidentally committed, treat it as compromised immediately and rotate it.

Secrets live outside the repo in `/Users/jacobfreund/kalshi-demo-key/` (demo private key + source configs) and are referenced by absolute path from the gitignored `config-demo.json` / `config.json`.

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

## Pull Requests

Every PR description **must begin with a `## Review order` section** stating whether the PR depends on any other open PR:
- If independent: `**No dependencies — review in any order.**` plus a one-line justification (e.g., no shared files with other open PRs).
- If dependent: name the PR(s) to review/merge first (e.g., `**Review #28 first** — this PR is stacked on it; merging out of order forces a rebase before the diff reads cleanly`), and say why (stacked branch, shared files, supersedes/duplicates another PR).

The reviewer should never have to figure out ordering themselves. When opening a new PR, check the currently open PRs for overlapping files or stacked branches and reflect that in the section. If a later PR changes the ordering of an existing open PR, update that PR's description too.

## Architecture Diagrams

Every major component has a Mermaid diagram. When adding a new component:
- Update `docs/architecture.md` with a Mermaid diagram showing where the new component fits in the overall system (PLAN.md stays lean; superseded diagrams live in `docs/archive/`).
- Diagrams use `graph TD` (top-down) for system architecture and `sequenceDiagram` for request/response flows.
- Keep diagrams focused: each one should show only the components relevant to that phase, with already-built components shown as grey/completed nodes using `style NodeName fill:#555`.
