#include "core/rate_limiter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

constexpr double kWriteTokensPerSecond = 100.0;
constexpr double kWriteCapacity = 100.0;
constexpr double kPlaceCost = 10.0;
constexpr int kDrainCalls = 10; // 10 x 10 = 100 = full capacity
constexpr auto kRefillAdvance = milliseconds{200};
constexpr auto kLongIdle = std::chrono::seconds{10};

std::shared_ptr<steady_clock::time_point> make_clock() {
  return std::make_shared<steady_clock::time_point>(steady_clock::now());
}

kalshi::RateLimiter::Clock
clock_fn(const std::shared_ptr<steady_clock::time_point> &now) {
  return [now] { return *now; };
}

void drain(kalshi::RateLimiter &limiter) {
  for (int call = 0; call < kDrainCalls; ++call) {
    (void)limiter.acquire(kPlaceCost);
  }
}

} // namespace

TEST(RateLimiterTest, FullBucketAllowsWithoutWait) {
  auto now = make_clock();
  kalshi::RateLimiter limiter{kWriteTokensPerSecond, kWriteCapacity,
                              clock_fn(now)};
  EXPECT_EQ(limiter.acquire(kPlaceCost), milliseconds{0});
}

TEST(RateLimiterTest, DrainingTheBucketForcesAWait) {
  auto now = make_clock();
  kalshi::RateLimiter limiter{kWriteTokensPerSecond, kWriteCapacity,
                              clock_fn(now)};
  drain(limiter);
  EXPECT_GT(limiter.acquire(kPlaceCost), milliseconds{0});
}

TEST(RateLimiterTest, WaitEqualsDeficitOverRate) {
  auto now = make_clock();
  kalshi::RateLimiter limiter{kWriteTokensPerSecond, kWriteCapacity,
                              clock_fn(now)};
  drain(limiter);
  constexpr double kFiftyTokens = 50.0; // 50 / 100 per s = 500ms
  EXPECT_EQ(limiter.acquire(kFiftyTokens), milliseconds{500});
}

TEST(RateLimiterTest, TokensRefillAsTheClockAdvances) {
  auto now = make_clock();
  kalshi::RateLimiter limiter{kWriteTokensPerSecond, kWriteCapacity,
                              clock_fn(now)};
  drain(limiter);
  EXPECT_GT(limiter.acquire(kPlaceCost), milliseconds{0});

  *now += kRefillAdvance; // 200ms x 100/s = 20 tokens refilled
  EXPECT_EQ(limiter.acquire(kPlaceCost), milliseconds{0});
}

TEST(RateLimiterTest, CapacityCapsAccumulatedTokens) {
  auto now = make_clock();
  kalshi::RateLimiter limiter{kWriteTokensPerSecond, kWriteCapacity,
                              clock_fn(now)};
  *now += kLongIdle; // would refill 1000 tokens, capped at 100
  drain(limiter);
  EXPECT_GT(limiter.acquire(kPlaceCost), milliseconds{0});
}
