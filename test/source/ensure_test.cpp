#include "ensure.hpp"

#include "logger.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <csignal>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// A throwing stub stands in for std::abort so the fail path can be exercised
// without killing the test process: ensure() invokes the abort action, the
// stub throws, and the test catches it. Production wires this to std::abort.
struct AbortRequested : std::exception {};

// Installs a capturing logger and returns the stream so tests can assert on the
// critical line ensure() emits.
std::shared_ptr<std::ostringstream> install_capture_logger() {
  auto stream = std::make_shared<std::ostringstream>();
  auto sink =
      std::make_shared<spdlog::sinks::ostream_sink_mt>(*stream, true /*flush*/);
  auto logger = std::make_shared<spdlog::logger>("ensure-test", sink);
  logger->set_level(spdlog::level::trace);
  logger->set_pattern("[%l] %v");
  kalshi::set_logger(logger);
  return stream;
}

// Fixture wiring a throwing abort and a fresh panic state before each case, and
// restoring the real logger / abort after, so cases stay independent.
class EnsureTest : public ::testing::Test {
protected:
  void SetUp() override {
    original_logger_ = kalshi::get_logger();
    kalshi::reset_panic_state();
    kalshi::set_panic_handler(nullptr);
    kalshi::set_abort_fn([] { throw AbortRequested{}; });
  }

  void TearDown() override {
    kalshi::set_panic_handler(nullptr);
    kalshi::set_abort_fn(nullptr); // restore the std::abort default
    kalshi::reset_panic_state();
    kalshi::set_logger(original_logger_);
  }

  std::shared_ptr<spdlog::logger> original_logger_;
};

} // namespace

TEST_F(EnsureTest, PassingConditionIsNoOp) {
  bool handler_ran = false;
  kalshi::set_panic_handler([&handler_ran] { handler_ran = true; });

  // A satisfied invariant must not log, flatten, or abort.
  EXPECT_NO_THROW(kalshi::ensure(true, "this holds"));
  EXPECT_FALSE(handler_ran);
}

TEST_F(EnsureTest, FailingConditionRunsPanicHandlerThenAborts) {
  bool handler_ran = false;
  kalshi::set_panic_handler([&handler_ran] { handler_ran = true; });

  EXPECT_THROW(kalshi::ensure(false, "book is corrupt"), AbortRequested);
  EXPECT_TRUE(handler_ran); // flatten ran before the abort
}

TEST_F(EnsureTest, FlattenHappensBeforeAbort) {
  // Record ordering: the panic handler (flatten) must run strictly before the
  // terminating action, otherwise we could die with quotes still resting.
  std::string order;
  kalshi::set_panic_handler([&order] { order += "flatten;"; });
  kalshi::set_abort_fn([&order] {
    order += "abort;";
    throw AbortRequested{};
  });

  EXPECT_THROW(kalshi::ensure(false, "invariant"), AbortRequested);
  EXPECT_EQ(order, "flatten;abort;");
}

TEST_F(EnsureTest, FailureLogsCriticalWithLocationAndMessage) {
  auto stream = install_capture_logger();
  kalshi::set_panic_handler(nullptr);

  EXPECT_THROW(kalshi::ensure(false, "best_bid exceeds best_ask"),
               AbortRequested);

  const std::string output = stream->str();
  EXPECT_NE(output.find("critical"), std::string::npos);
  EXPECT_NE(output.find("best_bid exceeds best_ask"), std::string::npos);
  // The source location should name this test file and a line number.
  EXPECT_NE(output.find("ensure_test.cpp"), std::string::npos);
}

TEST_F(EnsureTest, EmptyPanicHandlerStillAborts) {
  // No flatten hook registered: ensure must still terminate, never limp on.
  kalshi::set_panic_handler(nullptr);
  EXPECT_THROW(kalshi::ensure(false, "no handler"), AbortRequested);
}

TEST_F(EnsureTest, ReentrantEnsureSkipsSecondFlatten) {
  // An ensure firing *inside* the panic handler must not re-run the handler
  // (which could deadlock or recurse). The reentrant abort still fires.
  int flatten_calls = 0;
  kalshi::set_panic_handler([&flatten_calls] {
    ++flatten_calls;
    // A broken invariant discovered while flattening: must not re-enter
    // flatten.
    kalshi::ensure(false, "nested invariant during flatten");
  });

  EXPECT_THROW(kalshi::ensure(false, "outer invariant"), AbortRequested);
  EXPECT_EQ(flatten_calls, 1); // handler ran exactly once
}

// ---- Crash paths actually flatten ----

// A fatal signal skips stack unwinding, so the only way orders get cancelled is
// the installed signal handler running the panic hook. Verify in a forked child
// (death test) that raising SIGSEGV flattens before the process dies.
TEST(EnsureCrashTest, FatalSignalRunsPanicHandlerBeforeDeath) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH(
      {
        kalshi::reset_panic_state();
        kalshi::set_panic_handler(
            [] { std::fputs("FLATTENED-ON-CRASH\n", stderr); });
        kalshi::install_crash_flatten_handlers();
        std::raise(SIGSEGV);
      },
      "FLATTENED-ON-CRASH");
}

// std::terminate (e.g. an uncaught exception) likewise skips unwinding; the
// installed terminate handler must flatten first.
TEST(EnsureCrashTest, TerminateRunsPanicHandlerBeforeDeath) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH(
      {
        kalshi::reset_panic_state();
        kalshi::set_panic_handler(
            [] { std::fputs("FLATTENED-ON-TERMINATE\n", stderr); });
        kalshi::install_crash_flatten_handlers();
        std::terminate();
      },
      "FLATTENED-ON-TERMINATE");
}
