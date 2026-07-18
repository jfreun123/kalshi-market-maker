#include "engine/risk_limits.hpp"

#include <gtest/gtest.h>

TEST(RiskLimitsTest, DefaultsMatchNamedConstants) {
  const kalshi::RiskLimits config;

  EXPECT_EQ(config.max_position_per_market,
            kalshi::RiskLimits::kDefaultMaxPosition);
  EXPECT_EQ(config.max_open_orders_per_market,
            kalshi::RiskLimits::kDefaultMaxOpenOrders);
}
