#include "exchange/scanner_config.hpp"

#include <gtest/gtest.h>

TEST(ScannerConfigTest, DefaultsMatchNamedConstants) {
  const kalshi::ScannerConfig config;

  EXPECT_EQ(config.min_price_cents,
            kalshi::ScannerConfig::kDefaultMinPriceCents);
  EXPECT_EQ(config.max_price_cents,
            kalshi::ScannerConfig::kDefaultMaxPriceCents);
}
