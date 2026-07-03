# Developer Setup

## Prerequisites

| Tool | Version | Install |
|---|---|---|
| CMake | ≥ 3.14 | `apt-get install cmake` |
| Clang | ≥ 18 | `apt-get install clang-18` |
| clang-tidy | ≥ 18 | `apt-get install clang-tidy` |
| clang-format | ≥ 18 | `apt-get install clang-format-18` |
| Ninja | any | `apt-get install ninja-build` |
| OpenSSL | 3.x | `apt-get install libssl-dev` |
| cppcheck | any | `apt-get install cppcheck` |

## First-time setup

```bash
git clone <repo>
cd kalshi-market-maker
bash scripts/install-hooks.sh   # installs pre-commit hook
cmake --preset=dev               # configure (generates build/compile_commands.json)
cmake --build --preset=dev       # build everything
ctest --preset=dev               # run all tests
```

## Common build commands

```bash
# Build only a specific target
cmake --build build --target auth_test

# Run tests with output on failure
ctest --preset=dev

# Fix formatting
cmake --build build -t format-fix

# Check formatting without fixing
cmake --build build -t format-check
```

## Sanitizer builds

Run these before any significant merge to catch memory and threading bugs:

```bash
# AddressSanitizer — catches heap/stack overflows, use-after-free
cmake --preset=asan && cmake --build --preset=asan && ctest --preset=asan

# ThreadSanitizer — catches data races
cmake --preset=tsan && cmake --build --preset=tsan && ctest --preset=tsan
```

## Pre-commit hook

The hook at `scripts/pre-commit` runs automatically on every `git commit`:

1. **clang-format** — checks all staged `.cpp`/`.hpp` files. Blocks commit on failure.
2. **clang-tidy** — checks staged `.cpp` files using `build/compile_commands.json`. Blocks commit on failure.

If you see clang-tidy warnings, fix the code. See `CLAUDE.md` for the policy.

```bash
# Re-install hook (e.g. after a fresh clone)
bash scripts/install-hooks.sh

# Run the hook manually without committing
bash .git/hooks/pre-commit
```

## Configuration & environment variables

Runtime credentials and endpoints are read from a JSON config file, not from
environment variables:

```bash
cp config.example.json config.json   # fill in api_key + private_key_path
```

The only environment variable the project reads is `KALSHI_DEMO_CONFIG`, used
by the demo conformance tests (built with `-DKALSHI_DEMO_TESTS=ON`) to locate a
demo-environment config file; it defaults to `config-demo.json` in the repo
root.

Config files containing real credentials (`config.json`, `config-demo.json`,
generated `*.trade.json`) are gitignored; never commit them.
