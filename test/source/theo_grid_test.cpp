#include "engine/theo_grid.hpp"

#include <gtest/gtest.h>

namespace {

// Grid: 3 time-to-close breakpoints x 3 price breakpoints.
//  ttc_hours: [0.5, 2.0, 8.0]
//  mid_cents: [20, 50, 80]
//
// Values chosen so bilinear interpolation is easy to verify by hand.
//
//        mid=20  mid=50  mid=80
// ttc=0.5   18     48     78
// ttc=2.0   20     50     80   (identity)
// ttc=8.0   22     52     82

constexpr double kMidLow = 20.0;
constexpr double kMidMid = 50.0;
constexpr double kMidHigh = 80.0;
constexpr double kTtcLow = 0.5;
constexpr double kTtcMid = 2.0;
constexpr double kTtcHigh = 8.0;

// Grid table values (mid - 2 / identity / mid + 2).
constexpr double kOffset = 2.0;
constexpr double kValLowLow = kMidLow - kOffset;    // 18
constexpr double kValLowMid = kMidMid - kOffset;    // 48
constexpr double kValLowHigh = kMidHigh - kOffset;  // 78
constexpr double kValHighLow = kMidLow + kOffset;   // 22
constexpr double kValHighMid = kMidMid + kOffset;   // 52
constexpr double kValHighHigh = kMidHigh + kOffset; // 82

// Interpolation test inputs and expected values.
constexpr double kMidInterp = 35.0; // midpoint between mid=20 and mid=50
constexpr double kTtcInterp = 1.25; // midpoint (by fraction) in [0.5, 2.0]
constexpr double kExpectedMidInterp = 35.0; // identity row → value == mid
constexpr double kExpectedTtcInterp = 49.0; // lerp fraction=0.5: 48+0.5*(50-48)

// Out-of-range clamp inputs.
constexpr double kTtcBelowMin = 0.1;
constexpr double kTtcAboveMax = 100.0;
constexpr double kMidBelowMin = 5.0;
constexpr double kMidAboveMax = 95.0;

// Range for default-config sanity check.
constexpr double kTheoMin = 1.0;
constexpr double kTheoMax = 99.0;

kalshi::TheoGridConfig make_config() {
  kalshi::TheoGridConfig cfg;
  cfg.ttc_breakpoints = {kTtcLow, kTtcMid, kTtcHigh};
  cfg.mid_breakpoints = {kMidLow, kMidMid, kMidHigh};
  cfg.values = {
      {kValLowLow, kValLowMid, kValLowHigh},
      {kMidLow, kMidMid, kMidHigh},
      {kValHighLow, kValHighMid, kValHighHigh},
  };
  return cfg;
}

} // namespace

// ---- Exact grid points ----

TEST(TheoGridTest, ExactGridPointMidLowTtcLow) {
  kalshi::TheoGrid grid{make_config()};
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcLow, kMidLow), kValLowLow);
}

TEST(TheoGridTest, ExactGridPointMidMidTtcMid) {
  kalshi::TheoGrid grid{make_config()};
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcMid, kMidMid), kMidMid);
}

TEST(TheoGridTest, ExactGridPointMidHighTtcHigh) {
  kalshi::TheoGrid grid{make_config()};
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcHigh, kMidHigh), kValHighHigh);
}

// ---- Interpolation along mid axis (fixed ttc=2.0 row = identity) ----

TEST(TheoGridTest, InterpolateMidAxisAtIdentityRow) {
  kalshi::TheoGrid grid{make_config()};
  // At ttc=2.0, values are [20, 50, 80]. mid=35 is halfway between 20 and 50,
  // so the result is 35.0 (bilinear reduces to linear on the identity row).
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcMid, kMidInterp), kExpectedMidInterp);
}

// ---- Interpolation along ttc axis (fixed mid=50 column) ----

TEST(TheoGridTest, InterpolateTtcAxisAtMidColumn) {
  kalshi::TheoGrid grid{make_config()};
  // At mid=50, column values are [48, 50, 52].
  // ttc=1.25: fraction = (1.25-0.5)/(2.0-0.5) = 0.5 → 48 + 0.5*(50-48) = 49.
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcInterp, kMidMid), kExpectedTtcInterp);
}

// ---- Clamp below lower bound ----

TEST(TheoGridTest, ClampTtcBelowMinBound) {
  kalshi::TheoGrid grid{make_config()};
  // ttc < 0.5 → clamp to first row; at mid=50 that's kValLowMid (48).
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcBelowMin, kMidMid), kValLowMid);
}

TEST(TheoGridTest, ClampMidBelowMinBound) {
  kalshi::TheoGrid grid{make_config()};
  // mid < 20 → clamp to first column; at ttc=2.0 that's kMidLow (20).
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcMid, kMidBelowMin), kMidLow);
}

// ---- Clamp above upper bound ----

TEST(TheoGridTest, ClampTtcAboveMaxBound) {
  kalshi::TheoGrid grid{make_config()};
  // ttc > 8.0 → clamp to last row; at mid=50 that's kValHighMid (52).
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcAboveMax, kMidMid), kValHighMid);
}

TEST(TheoGridTest, ClampMidAboveMaxBound) {
  kalshi::TheoGrid grid{make_config()};
  // mid > 80 → clamp to last column; at ttc=2.0 that's kMidHigh (80).
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcMid, kMidAboveMax), kMidHigh);
}

// ---- Default config is non-empty and produces plausible values ----

TEST(TheoGridTest, DefaultConfigLookupIsInRange) {
  kalshi::TheoGrid grid{kalshi::TheoGridConfig::default_config()};
  const double theo = grid.lookup(kTtcMid, kMidMid);
  EXPECT_GE(theo, kTheoMin);
  EXPECT_LE(theo, kTheoMax);
}

// ---- Degenerate single-breakpoint axes ----

TEST(TheoGridTest, SingleBreakpointBothAxesReturnsOnlyValue) {
  kalshi::TheoGridConfig cfg;
  cfg.ttc_breakpoints = {kTtcMid};
  cfg.mid_breakpoints = {kMidMid};
  cfg.values = {{kValLowMid}};
  kalshi::TheoGrid grid{cfg};

  EXPECT_DOUBLE_EQ(grid.lookup(kTtcBelowMin, kMidBelowMin), kValLowMid);
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcMid, kMidMid), kValLowMid);
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcAboveMax, kMidAboveMax), kValLowMid);
}

TEST(TheoGridTest, SingleTtcBreakpointInterpolatesAlongMidAxis) {
  kalshi::TheoGridConfig cfg;
  cfg.ttc_breakpoints = {kTtcMid};
  cfg.mid_breakpoints = {kMidLow, kMidMid, kMidHigh};
  cfg.values = {{kMidLow, kMidMid, kMidHigh}};
  kalshi::TheoGrid grid{cfg};

  EXPECT_DOUBLE_EQ(grid.lookup(kTtcBelowMin, kMidInterp), kExpectedMidInterp);
  EXPECT_DOUBLE_EQ(grid.lookup(kTtcAboveMax, kMidMid), kMidMid);
}
