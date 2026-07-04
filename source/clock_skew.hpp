#pragma once
// Local-vs-server clock skew measurement from an HTTP Date response header.
// Kalshi rejects signed requests whose timestamp drifts from server time
// (header_timestamp_expired), so an unnoticed skewed clock silently kills all
// authenticated trading (demo finding, 2026-07-03: 12m40s drift 401'd every
// request). parse_http_date reads the RFC 1123 GMT format every HTTP response
// carries; clock_skew_seconds returns local minus server time.

#include <chrono>
#include <optional>
#include <string>

namespace kalshi {

using SystemTimePoint = std::chrono::system_clock::time_point;

std::optional<SystemTimePoint> parse_http_date(const std::string &http_date);

std::optional<std::chrono::seconds>
clock_skew_seconds(const std::string &http_date, SystemTimePoint local_now);

} // namespace kalshi
