#include "core/ensure.hpp"

#include "core/logger.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <utility>

namespace kalshi {

namespace {

// Process-wide hooks, mirroring the logger's global-with-injection pattern.
std::function<void()> &panic_handler() {
  static std::function<void()> handler;
  return handler;
}

std::function<void()> &abort_fn() {
  static std::function<void()> action{[] { std::abort(); }};
  return action;
}

// One-shot guard: the first failure runs the flatten hook; any reentrant
// failure (e.g. an ensure tripped *inside* the handler, or a fatal signal
// raised by the abort itself) skips the re-flatten so we cannot deadlock or
// recurse into the handler.
std::atomic<bool> g_panicking{false};

} // namespace

void set_panic_handler(std::function<void()> handler) {
  panic_handler() = std::move(handler);
}

void set_abort_fn(std::function<void()> action) {
  abort_fn() =
      action ? std::move(action) : std::function<void()>{[] { std::abort(); }};
}

void reset_panic_state() { g_panicking.store(false); }

void run_panic_handler() {
  // Only the first caller flattens; later callers (reentrancy, second thread,
  // the terminate/signal paths chaining after an abort) are no-ops.
  if (g_panicking.exchange(true)) {
    return;
  }
  const auto &handler = panic_handler();
  if (!handler) {
    return;
  }
  try {
    handler();
  } catch (...) {
    // Best-effort: a throwing flatten must never prevent the abort that
    // follows.
  }
}

namespace {

// Fatal signals and std::terminate skip stack unwinding, so the RAII
// cancel-on-exit guard never runs. Cancel resting orders here instead, then die
// normally so we still get the real signal's exit status / core dump.
//
// run_panic_handler -> cancel_all_quotes does HTTP/malloc/locking and is NOT
// async-signal-safe; this is a best-effort flatten on a process that is already
// crashing. The one-shot guard inside run_panic_handler keeps a fault *during*
// the flatten from looping. Better to risk an unsafe cancel than leave live
// quotes resting on the exchange unsupervised.
extern "C" void handle_fatal_signal(int sig) {
  run_panic_handler();
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

[[noreturn]] void panic_terminate() {
  run_panic_handler();
  std::abort();
}

} // namespace

void install_crash_flatten_handlers() {
  std::signal(SIGSEGV, handle_fatal_signal);
  std::signal(SIGABRT, handle_fatal_signal);
  std::signal(SIGFPE, handle_fatal_signal);
  std::signal(SIGILL, handle_fatal_signal);
  std::signal(SIGBUS, handle_fatal_signal);
  std::set_terminate(panic_terminate);
}

void ensure(bool condition, std::string_view what, std::source_location loc) {
  if (condition) {
    return;
  }
  get_logger()->critical("ENSURE FAILED {}:{} — {}", loc.file_name(),
                         loc.line(), what);
  run_panic_handler();
  abort_fn()();
  // The abort action is expected to terminate. If an injected stub returns
  // instead of throwing/aborting, fall back to std::abort so a failed invariant
  // can never silently continue.
  std::abort();
}

} // namespace kalshi
