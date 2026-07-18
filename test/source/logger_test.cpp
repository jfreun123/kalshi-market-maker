#include "core/logger.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <sstream>
#include <string>

namespace {

// Replaces the global kalshi logger with one backed by an ostringstream.
// Returns both the stream (for assertions) and the replacement logger.
std::pair<std::ostringstream, std::shared_ptr<spdlog::logger>>
make_capture_logger() {
  std::ostringstream stream;
  auto sink =
      std::make_shared<spdlog::sinks::ostream_sink_mt>(stream, true /*flush*/);
  auto logger = std::make_shared<spdlog::logger>("test", sink);
  logger->set_level(spdlog::level::trace);
  // Minimal pattern so assertions don't depend on timestamps.
  logger->set_pattern("[%l] %v");
  return {std::move(stream), std::move(logger)};
}

} // namespace

// ---- Logger accessor ----

TEST(LoggerTest, GetLoggerReturnsNonNull) {
  EXPECT_NE(kalshi::get_logger(), nullptr);
}

// ---- set_logger / get_logger round-trip ----

TEST(LoggerTest, SetLoggerReplacesLogger) {
  auto original = kalshi::get_logger();

  auto [stream, capture] = make_capture_logger();
  kalshi::set_logger(capture);
  EXPECT_EQ(kalshi::get_logger(), capture);

  // Restore so other tests are unaffected.
  kalshi::set_logger(original);
}

// ---- Log output reaches the sink ----

TEST(LoggerTest, LogInfoMessageAppearsInSink) {
  auto original = kalshi::get_logger();

  auto sink_stream = std::make_shared<std::ostringstream>();
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*sink_stream,
                                                               true /*flush*/);
  auto capture = std::make_shared<spdlog::logger>("capture", sink);
  capture->set_level(spdlog::level::trace);
  capture->set_pattern("%v");
  kalshi::set_logger(capture);

  kalshi::get_logger()->info("order placed ticker={}", "KXBTCD");

  const std::string output = sink_stream->str();
  EXPECT_NE(output.find("order placed ticker=KXBTCD"), std::string::npos);

  kalshi::set_logger(original);
}

TEST(LoggerTest, LogWarnMessageAppearsInSink) {
  auto original = kalshi::get_logger();

  auto sink_stream = std::make_shared<std::ostringstream>();
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*sink_stream,
                                                               true /*flush*/);
  auto capture = std::make_shared<spdlog::logger>("capture", sink);
  capture->set_level(spdlog::level::trace);
  capture->set_pattern("[%l] %v");
  kalshi::set_logger(capture);

  kalshi::get_logger()->warn("halted constraint={}", "kManualHalt");

  const std::string output = sink_stream->str();
  EXPECT_NE(output.find("warning"), std::string::npos);
  EXPECT_NE(output.find("halted constraint=kManualHalt"), std::string::npos);

  kalshi::set_logger(original);
}
