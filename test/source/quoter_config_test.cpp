#include "strategy/quoter_config.hpp"

#include <gtest/gtest.h>

TEST(QuoterConfigTest, DefaultsMatchNamedConstants) {
  const kalshi::QuoterConfig config;

  EXPECT_EQ(config.target_spread_cents,
            kalshi::QuoterConfig::kDefaultTargetSpreadCents);
  EXPECT_EQ(config.quote_size, kalshi::QuoterConfig::kDefaultQuoteSize);
}
