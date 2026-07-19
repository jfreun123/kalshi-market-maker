#include "engine/drift_estimator.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

constexpr double kFlatMid = 50.0;
constexpr double kSlopeCentsPerSecond = 0.1;
constexpr double kExpectedCentsPerMinute = 6.0;
constexpr double kSlopeTolerance = 0.05;
constexpr double kSignificanceThreshold = 2.0;
constexpr int kSampleSpacingSeconds = 5;
constexpr int kSampleCount = 20;
const std::string kTicker = "KXDRIFT";

kalshi::DriftEstimatorConfig wide_window() {
  kalshi::DriftEstimatorConfig config;
  config.window_seconds = 300;
  return config;
}

std::chrono::system_clock::time_point at_seconds(int seconds) {
  return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

} // namespace

TEST(DriftEstimatorTest, FlatSeriesHasNoSignificantDrift) {
  kalshi::DriftEstimator estimator{wide_window()};
  for (int index = 0; index < kSampleCount; ++index) {
    estimator.add_sample(kTicker, at_seconds(index * kSampleSpacingSeconds),
                         kFlatMid);
  }

  const auto signal = estimator.signal(
      kTicker, at_seconds(kSampleCount * kSampleSpacingSeconds));

  ASSERT_TRUE(signal.has_value());
  EXPECT_NEAR(signal->slope_cents_per_minute, 0.0, kSlopeTolerance);
  EXPECT_LT(std::abs(signal->t_stat), kSignificanceThreshold);
}

TEST(DriftEstimatorTest, LinearTrendRecoversSlopeAndIsSignificant) {
  kalshi::DriftEstimator estimator{wide_window()};
  for (int index = 0; index < kSampleCount; ++index) {
    const int seconds = index * kSampleSpacingSeconds;
    estimator.add_sample(kTicker, at_seconds(seconds),
                         kFlatMid + kSlopeCentsPerSecond * seconds);
  }

  const auto signal = estimator.signal(
      kTicker, at_seconds(kSampleCount * kSampleSpacingSeconds));

  ASSERT_TRUE(signal.has_value());
  EXPECT_NEAR(signal->slope_cents_per_minute, kExpectedCentsPerMinute,
              kSlopeTolerance);
  EXPECT_GE(std::abs(signal->t_stat), kSignificanceThreshold);
}

TEST(DriftEstimatorTest, AlternatingChopIsInsignificant) {
  kalshi::DriftEstimator estimator{wide_window()};
  for (int index = 0; index < kSampleCount; ++index) {
    const double wiggle = (index % 2 == 0) ? 1.0 : -1.0;
    estimator.add_sample(kTicker, at_seconds(index * kSampleSpacingSeconds),
                         kFlatMid + wiggle);
  }

  const auto signal = estimator.signal(
      kTicker, at_seconds(kSampleCount * kSampleSpacingSeconds));

  ASSERT_TRUE(signal.has_value());
  EXPECT_LT(std::abs(signal->t_stat), kSignificanceThreshold);
}

TEST(DriftEstimatorTest, SamplesOutsideWindowAreEvicted) {
  kalshi::DriftEstimatorConfig config;
  config.window_seconds = 60;
  kalshi::DriftEstimator estimator{config};
  for (int index = 0; index < kSampleCount; ++index) {
    const int seconds = index * kSampleSpacingSeconds;
    estimator.add_sample(kTicker, at_seconds(seconds), kFlatMid - seconds);
  }
  constexpr int kRecentStartSeconds = 200;
  for (int index = 0; index < kSampleCount; ++index) {
    estimator.add_sample(
        kTicker,
        at_seconds(kRecentStartSeconds + index * kSampleSpacingSeconds),
        kFlatMid);
  }

  const auto signal = estimator.signal(
      kTicker,
      at_seconds(kRecentStartSeconds + kSampleCount * kSampleSpacingSeconds));

  ASSERT_TRUE(signal.has_value());
  EXPECT_NEAR(signal->slope_cents_per_minute, 0.0, kSlopeTolerance);
}

TEST(DriftEstimatorTest, TooFewSamplesYieldsNoSignal) {
  kalshi::DriftEstimator estimator{wide_window()};
  constexpr int kFewSamples = 5;
  for (int index = 0; index < kFewSamples; ++index) {
    estimator.add_sample(kTicker, at_seconds(index * kSampleSpacingSeconds),
                         kFlatMid);
  }

  EXPECT_FALSE(
      estimator.signal(kTicker, at_seconds(kFewSamples * kSampleSpacingSeconds))
          .has_value());
}

TEST(DriftEstimatorTest, ForgetDropsTickerHistory) {
  kalshi::DriftEstimator estimator{wide_window()};
  for (int index = 0; index < kSampleCount; ++index) {
    estimator.add_sample(kTicker, at_seconds(index * kSampleSpacingSeconds),
                         kFlatMid);
  }
  estimator.forget(kTicker);

  EXPECT_FALSE(
      estimator
          .signal(kTicker, at_seconds(kSampleCount * kSampleSpacingSeconds))
          .has_value());
}