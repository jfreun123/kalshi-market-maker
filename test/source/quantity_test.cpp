#include "core/quantity.hpp"

#include <cstdint>
#include <gtest/gtest.h>

using kalshi::Quantity;

constexpr std::int64_t kTenContractsCenti = 1000;

TEST(QuantityTest, DefaultIsZero) {
  const Quantity quantity;
  EXPECT_TRUE(quantity.is_zero());
  EXPECT_EQ(quantity.centi(), 0);
}

TEST(QuantityTest, FromContractsScalesByHundred) {
  EXPECT_EQ(Quantity::from_contracts(10).centi(), kTenContractsCenti);
}

TEST(QuantityTest, FromFpStringParsesTwoDecimals) {
  EXPECT_EQ(Quantity::from_fp_string("10.50").centi(), 1050);
  EXPECT_EQ(Quantity::from_fp_string("0.16").centi(), 16);
  EXPECT_EQ(Quantity::from_fp_string("278.00").centi(), 27800);
}

TEST(QuantityTest, FromFpStringParsesNegative) {
  EXPECT_EQ(Quantity::from_fp_string("-1.47").centi(), -147);
  EXPECT_EQ(Quantity::from_fp_string("-54.00").centi(), -5400);
}

TEST(QuantityTest, SubUnitValueSurvivesInsteadOfRoundingToZero) {
  const Quantity small = Quantity::from_fp_string("0.16");
  EXPECT_FALSE(small.is_zero());
  EXPECT_EQ(small.centi(), 16);
}

TEST(QuantityTest, ContractsReturnsFractionalDouble) {
  EXPECT_DOUBLE_EQ(Quantity::from_fp_string("10.50").contracts(), 10.5);
}

TEST(QuantityTest, ToFpStringRoundTrips) {
  EXPECT_EQ(Quantity::from_fp_string("10.50").to_fp_string(), "10.50");
  EXPECT_EQ(Quantity::from_contracts(7).to_fp_string(), "7.00");
  EXPECT_EQ(Quantity::from_fp_string("0.16").to_fp_string(), "0.16");
  EXPECT_EQ(Quantity::from_fp_string("-54.00").to_fp_string(), "-54.00");
}

TEST(QuantityTest, AccumulatesSignedDeltasExactly) {
  Quantity running = Quantity::from_contracts(300);
  running += Quantity::from_fp_string("-1.47");
  running += Quantity::from_fp_string("-0.16");
  EXPECT_EQ(running.centi(), 30000 - 147 - 16);
}

TEST(QuantityTest, UnaryNegationFlipsSign) {
  EXPECT_EQ((-Quantity::from_contracts(5)).centi(), -500);
}

TEST(QuantityTest, AdditionAndSubtraction) {
  const Quantity sum =
      Quantity::from_contracts(3) + Quantity::from_contracts(4);
  EXPECT_EQ(sum.centi(), 700);
  const Quantity diff =
      Quantity::from_contracts(3) - Quantity::from_contracts(4);
  EXPECT_EQ(diff.centi(), -100);
}

TEST(QuantityTest, Comparisons) {
  EXPECT_TRUE(Quantity::from_contracts(1) < Quantity::from_contracts(2));
  EXPECT_TRUE(Quantity::from_contracts(2) >= Quantity::from_contracts(2));
  EXPECT_TRUE(Quantity::from_contracts(2) == Quantity::from_fp_string("2.00"));
  EXPECT_TRUE(Quantity::from_contracts(2) != Quantity::from_contracts(3));
}

TEST(QuantityTest, AbsAndMin) {
  EXPECT_EQ(kalshi::abs(Quantity::from_contracts(-4)).centi(), 400);
  EXPECT_EQ(
      kalshi::min(Quantity::from_contracts(2), Quantity::from_contracts(5))
          .centi(),
      200);
}
