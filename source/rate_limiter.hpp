#pragma once

// Token-bucket rate limiter. Refills continuously at a per-second rate up to a
// capacity; acquire(cost) reserves tokens and returns how long the caller must
// wait before spending them to stay within budget (0 when tokens are on hand).
// Mirrors Kalshi's server-side write budget so the client throttles itself
// instead of taking 429s. The clock is injectable for deterministic tests.

#include <chrono>
#include <functional>

namespace kalshi {

class RateLimiter {
public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — rate vs capacity
  RateLimiter(double tokens_per_second, double capacity, Clock clock = {});

  [[nodiscard]] std::chrono::milliseconds acquire(double cost);

private:
  double rate_;
  double capacity_;
  double tokens_;
  Clock clock_;
  std::chrono::steady_clock::time_point last_refill_;
};

} // namespace kalshi
