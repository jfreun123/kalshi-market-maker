#pragma once

// Fail-fast invariant checks. `ensure` asserts a condition that MUST hold for
// safe trading; on violation it flattens (cancels all resting orders) via a
// registered panic handler and then terminates the process. An inconsistent
// process must never keep quoting — fail loud and flat, never limp on.
//
// This is distinct from a risk halt: a halt is recoverable (resume()); an
// `ensure` violation is an unrecoverable invariant break. Use it only for
// "this should be impossible" conditions, never for expected error handling
// (those stay exceptions / order-check rejections).

#include <functional>
#include <source_location>
#include <string_view>

namespace kalshi {

// Registers the panic handler — the action `ensure` runs to flatten before the
// process dies (main wires this to TradingSession::cancel_all_quotes). The
// handler MUST be best-effort and never throw; it also runs from the
// terminate/fatal-signal paths, so it cannot rely on stack unwinding. Pass
// nullptr to clear.
void set_panic_handler(std::function<void()> handler);

// Runs the registered panic handler at most once (guarded against reentrancy),
// swallowing any exception it throws. Safe to call from std::set_terminate and
// fatal-signal handlers so resting orders are cancelled even on a crash.
void run_panic_handler();

// Installs the flatten-on-crash hooks: fatal-signal handlers (SIGSEGV, SIGABRT,
// SIGFPE, SIGILL, SIGBUS) and std::set_terminate, all of which run the panic
// handler before the process dies. These paths skip stack unwinding, so the
// usual RAII cancel-on-exit never fires — this is how resting orders get
// cancelled on a crash or an ensure() failure. Call once, after the panic
// handler is registered, in any mode that places real orders.
void install_crash_flatten_handlers();

// Replaces the terminating action `ensure` invokes after flattening. Defaults
// to std::abort. Tests override it with a recording/throwing stub so the fail
// path can be exercised without killing the process; pass nullptr to restore
// the std::abort default.
void set_abort_fn(std::function<void()> abort_fn);

// Clears the one-shot "already panicking" guard. Intended for tests so cases
// that exercise the fail path stay independent.
void reset_panic_state();

// Asserts an invariant. If `condition` is false: log `critical` with the source
// location and `what`, run the panic handler (flatten) at most once, then
// invoke the abort action.
void ensure(bool condition, std::string_view what,
            std::source_location loc = std::source_location::current());

} // namespace kalshi
