#include "rate_limiter.hpp"

#include <algorithm>
#include <utility>

namespace kalshi {

RateLimiter::RateLimiter(double tokens_per_second, double capacity, Clock clock)
    : rate_{tokens_per_second}, capacity_{capacity}, tokens_{capacity},
      clock_{clock ? std::move(clock)
                   : Clock{[] { return std::chrono::steady_clock::now(); }}},
      last_refill_{clock_()} {}

std::chrono::milliseconds RateLimiter::acquire(double cost) {
  const auto now = clock_();
  const double elapsed_seconds =
      std::chrono::duration<double>(now - last_refill_).count();
  tokens_ = std::min(capacity_, tokens_ + (elapsed_seconds * rate_));
  last_refill_ = now;

  const double deficit = cost - tokens_;
  tokens_ -= cost;
  if (deficit <= 0.0 || rate_ <= 0.0) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::duration<double>(deficit / rate_));
}

} // namespace kalshi
