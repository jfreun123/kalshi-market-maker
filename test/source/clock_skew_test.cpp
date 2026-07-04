#include "clock_skew.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

const std::string kHttpDate = "Fri, 03 Jul 2026 19:37:09 GMT";
constexpr long long kHttpDateEpochSeconds = 1'783'107'429LL;
constexpr auto kObservedDemoDrift = std::chrono::seconds{760};

kalshi::SystemTimePoint time_point_at_epoch(long long epoch_seconds) {
  return kalshi::SystemTimePoint{std::chrono::seconds{epoch_seconds}};
}

} // namespace

TEST(ClockSkewTest, ParsesRfc1123HttpDate) {
  const auto parsed = kalshi::parse_http_date(kHttpDate);

  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(
                parsed->time_since_epoch())
                .count(),
            kHttpDateEpochSeconds);
}

TEST(ClockSkewTest, RejectsMalformedDate) {
  EXPECT_FALSE(kalshi::parse_http_date("").has_value());
  EXPECT_FALSE(kalshi::parse_http_date("not a date").has_value());
  EXPECT_FALSE(kalshi::parse_http_date("2026-07-03T19:37:09Z").has_value());
}

TEST(ClockSkewTest, ZeroSkewWhenClocksAgree) {
  const auto skew = kalshi::clock_skew_seconds(
      kHttpDate, time_point_at_epoch(kHttpDateEpochSeconds));

  ASSERT_TRUE(skew.has_value());
  EXPECT_EQ(skew->count(), 0);
}

TEST(ClockSkewTest, PositiveSkewWhenLocalClockAhead) {
  const auto local_now =
      time_point_at_epoch(kHttpDateEpochSeconds) + kObservedDemoDrift;

  const auto skew = kalshi::clock_skew_seconds(kHttpDate, local_now);

  ASSERT_TRUE(skew.has_value());
  EXPECT_EQ(*skew, kObservedDemoDrift);
}

TEST(ClockSkewTest, NegativeSkewWhenLocalClockBehind) {
  const auto local_now =
      time_point_at_epoch(kHttpDateEpochSeconds) - kObservedDemoDrift;

  const auto skew = kalshi::clock_skew_seconds(kHttpDate, local_now);

  ASSERT_TRUE(skew.has_value());
  EXPECT_EQ(*skew, -kObservedDemoDrift);
}

TEST(ClockSkewTest, NoSkewForUnparseableDate) {
  EXPECT_FALSE(kalshi::clock_skew_seconds(
                   "garbage", time_point_at_epoch(kHttpDateEpochSeconds))
                   .has_value());
}
