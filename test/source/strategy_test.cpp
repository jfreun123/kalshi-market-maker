#include "strategy.hpp"

#include "fake_order_manager.hpp"
#include "fake_strategy.hpp"
#include "quoter.hpp"
#include "risk_manager.hpp"
#include "trading_session.hpp"
#include "types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace {

const std::string kTicker = "KXSEAM-26-T1";
const std::string kOrderId = "order-seam-001";
constexpr int kYesBid = 51;
constexpr int kNoBid = 47;
constexpr int kLevelContracts = 100;
constexpr int kFillPrice = 50;
constexpr int kFillContracts = 5;
constexpr auto kPastFeedGrace =
    kalshi::TradingSession::kFeedConfirmGrace + std::chrono::seconds{1};

kalshi::Orderbook make_orderbook(const std::string &ticker) {
  kalshi::Orderbook book;
  book.ticker = ticker;
  book.yes = {kalshi::Level{kYesBid,
                            kalshi::Quantity::from_contracts(kLevelContracts)}};
  book.no = {
      kalshi::Level{kNoBid, kalshi::Quantity::from_contracts(kLevelContracts)}};
  return book;
}

kalshi::Fill make_fill(const std::string &ticker, const std::string &order_id) {
  kalshi::Fill fill;
  fill.order_id = order_id;
  fill.market_ticker = ticker;
  fill.side = kalshi::Side::Yes;
  fill.price_cents = kFillPrice;
  fill.quantity = kalshi::Quantity::from_contracts(kFillContracts);
  fill.timestamp = std::chrono::system_clock::time_point{};
  return fill;
}

struct SeamFixture {
  FakeOrderManager order_mgr;
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  FakeStrategy strategy;
  kalshi::TradingSession session{std::vector<std::string>{kTicker}, order_mgr,
                                 risk_mgr, strategy};
};

} // namespace

TEST(StrategySeamTest, QuoterImplementsIStrategy) {
  static_assert(std::is_base_of_v<kalshi::IStrategy, kalshi::Quoter>);

  FakeOrderManager order_mgr;
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  kalshi::IStrategy &strategy = quoter;
  strategy.set_reduce_only(false);
}

TEST(StrategySeamTest, DeltaDrivesStrategyUpdate) {
  SeamFixture fixture;

  fixture.session.on_snapshot(make_orderbook(kTicker));
  fixture.session.on_delta(kTicker, kalshi::Side::Yes, kYesBid - 1,
                           kalshi::Quantity::from_contracts(kLevelContracts));

  ASSERT_FALSE(fixture.strategy.updated_tickers.empty());
  EXPECT_EQ(fixture.strategy.updated_tickers.back(), kTicker);
}

TEST(StrategySeamTest, FullFillForgetsOrder) {
  SeamFixture fixture;

  fixture.session.on_fill(make_fill(kTicker, kOrderId));

  ASSERT_EQ(fixture.strategy.forgotten_orders.size(), 1U);
  EXPECT_EQ(fixture.strategy.forgotten_orders.front(),
            kTicker + ":" + kOrderId);
}

TEST(StrategySeamTest, DisconnectResetsStrategyQuotes) {
  SeamFixture fixture;

  fixture.session.on_disconnect();

  EXPECT_EQ(fixture.strategy.reset_count, 1);
}

TEST(StrategySeamTest, FeedDeadCancelForgetsTicker) {
  FakeOrderManager order_mgr;
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  FakeStrategy strategy;
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::TradingSession session{
      std::vector<std::string>{kTicker}, order_mgr, risk_mgr, strategy, nullptr,
      [clock_now] { return *clock_now; }};

  session.seed_orderbook(make_orderbook(kTicker));
  *clock_now += kPastFeedGrace;
  session.enforce_quote_safety();

  ASSERT_EQ(strategy.forgotten_tickers.size(), 1U);
  EXPECT_EQ(strategy.forgotten_tickers.front(), kTicker);
}
