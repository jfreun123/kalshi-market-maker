#include "engine/order_manager_iface.hpp"

#include "fake_order_manager.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {
constexpr kalshi::Quantity kFakePosition = kalshi::Quantity::from_contracts(7);
const std::string kTicker = "KXIFACE-26-T1";
} // namespace

TEST(OrderManagerIfaceTest, FakeBindsThroughInterfaceReference) {
  FakeOrderManager fake;
  fake.positions[kTicker] = kFakePosition;

  kalshi::IOrderManager &order_mgr = fake;

  EXPECT_EQ(order_mgr.net_position(kTicker), kFakePosition);
  EXPECT_EQ(order_mgr.net_position("KXUNKNOWN-26-T1"), kalshi::Quantity{});
  EXPECT_TRUE(order_mgr.open_orders().empty());
}

TEST(OrderManagerIfaceTest, ExposureDecompositionDefaultsToFlat) {
  const kalshi::ExposureDecomposition exposure;

  EXPECT_DOUBLE_EQ(exposure.spread_capture_cents, 0.0);
  EXPECT_TRUE(exposure.net_inventory.is_zero());
  EXPECT_DOUBLE_EQ(exposure.e_win_cents, 0.0);
  EXPECT_DOUBLE_EQ(exposure.e_loss_cents, 0.0);
}
