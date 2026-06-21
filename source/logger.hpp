#pragma once

#include <spdlog/logger.h>

#include <memory>

namespace kalshi {

// Returns the process-wide kalshi logger. Never null after program start.
[[nodiscard]] std::shared_ptr<spdlog::logger> get_logger();

// Replaces the process-wide logger. Used in tests to inject a capturing sink.
void set_logger(std::shared_ptr<spdlog::logger> logger);

} // namespace kalshi
