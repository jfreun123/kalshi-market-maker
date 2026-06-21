#include "logger.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <mutex>

namespace kalshi {

namespace {

// NOLINT(cppcoreguidelines-avoid-non-const-global-variables) — mutable by
// design: set_logger() is the only mutation path.
std::mutex
    g_logger_mutex; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::shared_ptr<spdlog::logger>
    g_logger; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

std::shared_ptr<spdlog::logger> make_default_logger() {
  auto logger = spdlog::stdout_color_mt("kalshi");
  logger->set_level(spdlog::level::info);
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
  return logger;
}

} // namespace

std::shared_ptr<spdlog::logger> get_logger() {
  std::lock_guard<std::mutex> lock{g_logger_mutex};
  if (!g_logger) {
    g_logger = make_default_logger();
  }
  return g_logger;
}

void set_logger(std::shared_ptr<spdlog::logger> logger) {
  std::lock_guard<std::mutex> lock{g_logger_mutex};
  g_logger = std::move(logger);
}

} // namespace kalshi
