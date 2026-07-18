#include "engine/flow_imbalance_config.hpp"

#include <gtest/gtest.h>

TEST(FlowImbalanceConfigTest, DefaultsMatchNamedConstants) {
  const kalshi::FlowImbalanceConfig config;

  EXPECT_EQ(config.window_seconds,
            kalshi::FlowImbalanceConfig::kDefaultWindowSeconds);
}
