#include "auth.hpp"
#include "config.hpp"
#include "http_transport.hpp"
#include "orderbook.hpp"
#include "rest_client.hpp"
#include "types.hpp"
#include "websocket_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#ifndef KALSHI_DEMO_CONFIG_DEFAULT
#define KALSHI_DEMO_CONFIG_DEFAULT "config-demo.json"
#endif

namespace {

struct DemoCreds {
  kalshi::AppConfig config;
  std::string private_key_pem;
};

std::optional<DemoCreds> load_demo_creds() {
  const char *env = std::getenv("KALSHI_DEMO_CONFIG");
  const std::string config_path =
      env != nullptr ? env : KALSHI_DEMO_CONFIG_DEFAULT;

  std::ifstream config_file{config_path};
  if (!config_file) {
    return std::nullopt;
  }

  kalshi::AppConfig config;
  try {
    config = kalshi::load_config(config_path);
  } catch (const std::exception &) {
    return std::nullopt;
  }

  if (config.base_url.find("demo") == std::string::npos) {
    return std::nullopt;
  }

  std::ifstream key_file{config.private_key_path};
  if (!key_file) {
    return std::nullopt;
  }
  const std::string pem{std::istreambuf_iterator<char>{key_file},
                        std::istreambuf_iterator<char>{}};
  if (pem.find("PRIVATE KEY") == std::string::npos) {
    return std::nullopt;
  }

  return DemoCreds{std::move(config), pem};
}

constexpr const char *kSkipReason =
    "demo creds unavailable — set KALSHI_DEMO_CONFIG to a demo config whose "
    "base_url contains 'demo' and whose private_key_path is a readable PEM";

class DemoConformanceTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto creds = load_demo_creds();
    if (!creds) {
      GTEST_SKIP() << kSkipReason;
    }
    creds_ = std::move(*creds);
  }

  [[nodiscard]] kalshi::RestClient make_rest() const {
    return kalshi::RestClient{
        kalshi::Auth{creds_.config.api_key, creds_.private_key_pem},
        std::make_unique<kalshi::HttpTransport>(), creds_.config.base_url};
  }

  [[nodiscard]] std::string first_target_ticker() const {
    return creds_.config.target_tickers.empty()
               ? std::string{}
               : creds_.config.target_tickers.front();
  }

  DemoCreds creds_;
};

} // namespace

TEST_F(DemoConformanceTest, GetMarketsParses) {
  auto rest = make_rest();
  const auto markets = rest.get_markets();
  ASSERT_FALSE(markets.empty());
  EXPECT_FALSE(markets.front().ticker.empty());
}

TEST_F(DemoConformanceTest, GetOrderbookParsesIntoSaneBbo) {
  const std::string ticker = first_target_ticker();
  if (ticker.empty()) {
    GTEST_SKIP() << "demo config has no target_tickers to query";
  }
  auto rest = make_rest();
  const auto book = rest.get_orderbook(ticker);

  kalshi::LocalOrderbook local;
  local.apply_snapshot(book);
  const auto bid = local.best_bid();
  const auto ask = local.best_ask();
  if (bid.has_value() && ask.has_value()) {
    EXPECT_GT(ask->price_cents, 0);
    EXPECT_LT(ask->price_cents, kalshi::kPriceBasis);
    EXPECT_LE(bid->price_cents, ask->price_cents);
  }
}

TEST_F(DemoConformanceTest, GetPositionsParses) {
  auto rest = make_rest();
  EXPECT_NO_THROW((void)rest.get_positions());
}

TEST_F(DemoConformanceTest, GetOpenOrdersParses) {
  auto rest = make_rest();
  EXPECT_NO_THROW((void)rest.get_open_orders());
}

TEST_F(DemoConformanceTest, PlaceAndCancelOrderParses) {
  const std::string ticker = first_target_ticker();
  if (ticker.empty()) {
    GTEST_SKIP() << "demo config has no target_tickers to trade";
  }
  auto rest = make_rest();

  constexpr int kDeepPassiveBidCents = 1;
  constexpr int kOneContract = 1;
  const auto order =
      rest.place_order(ticker, kalshi::Side::Yes, kDeepPassiveBidCents,
                       kOneContract, kalshi::OrderType::Limit);

  EXPECT_FALSE(order.id.empty());
  EXPECT_EQ(order.price_cents, kDeepPassiveBidCents);
  EXPECT_TRUE(rest.cancel_order(order.id));
}

TEST_F(DemoConformanceTest, WebSocketOrderbookSnapshotParses) {
  const std::string ticker = first_target_ticker();
  if (ticker.empty()) {
    GTEST_SKIP() << "demo config has no target_tickers to subscribe";
  }

  kalshi::WebSocketClient ws{
      kalshi::Auth{creds_.config.api_key, creds_.private_key_pem},
      std::make_unique<kalshi::IxWebSocket>(), creds_.config.ws_url,
      /*max_reconnects=*/0, std::chrono::milliseconds{0}};

  std::atomic<bool> got_snapshot{false};
  ws.on_orderbook_snapshot([&got_snapshot](const kalshi::Orderbook &snapshot) {
    if (!snapshot.ticker.empty()) {
      got_snapshot.store(true);
    }
  });
  ws.subscribe(ticker);

  std::thread runner{[&ws] { ws.run(); }};

  constexpr int kMaxWaitTenths = 100; // up to 10s
  for (int tenth = 0; tenth < kMaxWaitTenths && !got_snapshot.load(); ++tenth) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  ws.stop();
  runner.join();

  EXPECT_TRUE(got_snapshot.load())
      << "no orderbook_snapshot received within 10s for ticker " << ticker;
}
