#include "engine/event_pump.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <variant>
#include <vector>

namespace {

constexpr auto kShortTimeout = std::chrono::milliseconds{50};
constexpr auto kLongTimeout = std::chrono::milliseconds{2000};
constexpr int kDeltaPrice = 42;
const std::string kTicker = "KXPUMP";

kalshi::SessionEvent make_delta() {
  return kalshi::BookDeltaEvent{kTicker, kalshi::Side::Yes, kDeltaPrice,
                                kalshi::Quantity::from_contracts(1)};
}

kalshi::SessionEvent make_fill() {
  kalshi::Fill fill;
  fill.trade_id = "pump-fill";
  fill.market_ticker = kTicker;
  return fill;
}

} // namespace

TEST(EventPumpTest, DrainReturnsEventsInArrivalOrder) {
  kalshi::EventPump pump;
  kalshi::Orderbook snapshot;
  snapshot.ticker = kTicker;
  pump.push(snapshot);
  pump.push(make_delta());
  pump.push(kalshi::PublicTrade{});
  pump.push(make_fill());
  pump.push(kalshi::DisconnectEvent{});

  std::vector<kalshi::SessionEvent> drained;
  ASSERT_TRUE(pump.wait_drain(drained, kShortTimeout));
  ASSERT_EQ(drained.size(), 5U);
  EXPECT_TRUE(std::holds_alternative<kalshi::Orderbook>(drained[0]));
  EXPECT_TRUE(std::holds_alternative<kalshi::BookDeltaEvent>(drained[1]));
  EXPECT_TRUE(std::holds_alternative<kalshi::PublicTrade>(drained[2]));
  EXPECT_TRUE(std::holds_alternative<kalshi::Fill>(drained[3]));
  EXPECT_TRUE(std::holds_alternative<kalshi::DisconnectEvent>(drained[4]));
  EXPECT_EQ(std::get<kalshi::BookDeltaEvent>(drained[1]).price_cents,
            kDeltaPrice);
}

TEST(EventPumpTest, WaitDrainWakesOnPush) {
  kalshi::EventPump pump;
  std::vector<kalshi::SessionEvent> drained;
  std::thread producer([&pump]() {
    std::this_thread::sleep_for(kShortTimeout);
    pump.push(kalshi::DisconnectEvent{});
  });

  const bool keep_running = pump.wait_drain(drained, kLongTimeout);
  producer.join();

  EXPECT_TRUE(keep_running);
  ASSERT_EQ(drained.size(), 1U);
}

TEST(EventPumpTest, TimeoutWithNoEventsKeepsRunning) {
  kalshi::EventPump pump;
  std::vector<kalshi::SessionEvent> drained;

  EXPECT_TRUE(pump.wait_drain(drained, kShortTimeout));
  EXPECT_TRUE(drained.empty());
}

TEST(EventPumpTest, StopWakesWaiterAndSignalsExit) {
  kalshi::EventPump pump;
  std::vector<kalshi::SessionEvent> drained;
  std::thread stopper([&pump]() {
    std::this_thread::sleep_for(kShortTimeout);
    pump.stop();
  });

  const bool keep_running = pump.wait_drain(drained, kLongTimeout);
  stopper.join();

  EXPECT_FALSE(keep_running);
  EXPECT_TRUE(drained.empty());
}

TEST(EventPumpTest, StopStillDeliversQueuedEventsFirst) {
  kalshi::EventPump pump;
  pump.push(make_delta());
  pump.push(make_fill());
  pump.stop();

  std::vector<kalshi::SessionEvent> drained;
  EXPECT_TRUE(pump.wait_drain(drained, kShortTimeout));
  EXPECT_EQ(drained.size(), 2U);
  EXPECT_FALSE(pump.wait_drain(drained, kShortTimeout));
}

TEST(EventPumpTest, HighWaterTracksMaxQueueDepth) {
  kalshi::EventPump pump;
  pump.push(make_delta());
  pump.push(make_delta());
  pump.push(make_delta());
  std::vector<kalshi::SessionEvent> drained;
  ASSERT_TRUE(pump.wait_drain(drained, kShortTimeout));
  pump.push(make_delta());

  EXPECT_EQ(pump.high_water(), 3U);
}