#include "adverse_selection.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

using Clock = std::chrono::system_clock;
using TP = Clock::time_point;

constexpr int kThreshold = 3; // trigger after 3 fills
constexpr double kWindowSec = 10.0;
constexpr double kCooldownSec = 5.0;

const std::string kTicker = "KXBTCD";
const std::string kOtherTicker = "KXETHU";

kalshi::AdverseSelectionConfig tight_config() {
  return kalshi::AdverseSelectionConfig{kThreshold, kWindowSec, kCooldownSec};
}

// Generates a series of timestamps spaced delta_seconds apart, starting at t0.
TP at(TP base_time, double delta_seconds) {
  return base_time + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>{delta_seconds});
}

} // namespace

// ---- Below threshold ----

TEST(AdverseSelectionTest, NoTriggerBelowThreshold) {
  kalshi::AdverseSelectionGuard guard{tight_config()};
  const TP now = Clock::now();
  // 2 fills — below threshold of 3.
  EXPECT_FALSE(guard.record_fill(kTicker, now));
  EXPECT_FALSE(guard.record_fill(kTicker, at(now, 1.0)));
}

// ---- At threshold ----

TEST(AdverseSelectionTest, TriggerAtThreshold) {
  kalshi::AdverseSelectionGuard guard{tight_config()};
  const TP now = Clock::now();
  guard.record_fill(kTicker, now);
  guard.record_fill(kTicker, at(now, 1.0));
  // Third fill hits the threshold.
  EXPECT_TRUE(guard.record_fill(kTicker, at(now, 2.0)));
}

// ---- Window expiry ----

TEST(AdverseSelectionTest, OldFillsExpireFromRollingWindow) {
  // threshold=3, window=10s. Record 2 fills at t=0, t=1, then 1 more at t=11.
  // The first 2 fall outside the 10s window when the 3rd arrives → no trigger.
  kalshi::AdverseSelectionGuard guard{tight_config()};
  const TP start = Clock::now();
  guard.record_fill(kTicker, start);
  guard.record_fill(kTicker, at(start, 1.0));
  // 11 seconds later — both earlier fills are outside the 10s window.
  EXPECT_FALSE(guard.record_fill(kTicker, at(start, 11.0)));
}

// ---- Reset ----

TEST(AdverseSelectionTest, ResetClearsFillHistory) {
  kalshi::AdverseSelectionGuard guard{tight_config()};
  const TP now = Clock::now();
  // Trigger the guard.
  guard.record_fill(kTicker, now);
  guard.record_fill(kTicker, at(now, 1.0));
  ASSERT_TRUE(guard.record_fill(kTicker, at(now, 2.0)));

  // After reset, fresh fills should not immediately re-trigger.
  guard.reset(kTicker);
  EXPECT_FALSE(guard.record_fill(kTicker, at(now, 3.0)));
  EXPECT_FALSE(guard.record_fill(kTicker, at(now, 4.0)));
}

// ---- Ticker isolation ----

TEST(AdverseSelectionTest, FillsAreTrackedPerTicker) {
  kalshi::AdverseSelectionGuard guard{tight_config()};
  const TP now = Clock::now();
  // Fill kTicker twice, kOtherTicker once.
  guard.record_fill(kTicker, now);
  guard.record_fill(kTicker, at(now, 1.0));
  guard.record_fill(kOtherTicker, at(now, 1.0));
  // Third fill on kTicker triggers, kOtherTicker is independent (still only 2
  // fills).
  EXPECT_TRUE(guard.record_fill(kTicker, at(now, 2.0)));
  EXPECT_FALSE(guard.record_fill(kOtherTicker, at(now, 2.0)));
}

// ---- Default config ----

TEST(AdverseSelectionTest, DefaultConfigHasExpectedValues) {
  const kalshi::AdverseSelectionConfig cfg;
  EXPECT_EQ(cfg.fill_threshold,
            kalshi::AdverseSelectionConfig::kDefaultFillThreshold);
  EXPECT_DOUBLE_EQ(cfg.window_seconds,
                   kalshi::AdverseSelectionConfig::kDefaultWindowSeconds);
  EXPECT_DOUBLE_EQ(cfg.cooldown_seconds,
                   kalshi::AdverseSelectionConfig::kDefaultCooldownSeconds);
}
