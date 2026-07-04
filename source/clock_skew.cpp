#include "clock_skew.hpp"

#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>

namespace kalshi {

std::optional<SystemTimePoint> parse_http_date(const std::string &http_date) {
  std::tm parsed{};
  std::istringstream stream{http_date};
  stream.imbue(std::locale::classic());
  stream >> std::get_time(&parsed, "%a, %d %b %Y %H:%M:%S GMT");
  if (stream.fail()) {
    return std::nullopt;
  }
  const std::time_t utc_seconds = timegm(&parsed);
  if (utc_seconds == static_cast<std::time_t>(-1)) {
    return std::nullopt;
  }
  return std::chrono::system_clock::from_time_t(utc_seconds);
}

std::optional<std::chrono::seconds>
clock_skew_seconds(const std::string &http_date, SystemTimePoint local_now) {
  const auto server_time = parse_http_date(http_date);
  if (!server_time.has_value()) {
    return std::nullopt;
  }
  return std::chrono::duration_cast<std::chrono::seconds>(local_now -
                                                          *server_time);
}

} // namespace kalshi
