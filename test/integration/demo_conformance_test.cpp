#include "auth.hpp"
#include "config.hpp"
#include "http_transport.hpp"
#include "orderbook.hpp"
#include "rest_client.hpp"
#include "ticker_scanner.hpp"
#include "types.hpp"
#include "websocket_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
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

constexpr auto kEventPollInterval = std::chrono::milliseconds{100};
constexpr auto kOpenOrderPollInterval = std::chrono::milliseconds{300};

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

  [[nodiscard]] kalshi::WebSocketClient make_ws() const {
    return kalshi::WebSocketClient{
        kalshi::Auth{creds_.config.api_key, creds_.private_key_pem},
        std::make_unique<kalshi::IxWebSocket>(), creds_.config.ws_url,
        /*max_reconnects=*/0, std::chrono::milliseconds{0}};
  }

  // A market must always exist on the demo exchange (Jacob, 2026-07-04):
  // prefer a scanner pick (live, quotable book), fall back to any open
  // market, and FAIL — never skip — if neither yields one.
  [[nodiscard]] std::string first_target_ticker() {
    if (!creds_.config.target_tickers.empty()) {
      return creds_.config.target_tickers.front();
    }
    auto rest = make_rest();
    kalshi::TickerScanner scanner{rest, creds_.config.scanner};
    const auto picks = scanner.scan(1);
    if (!picks.empty()) {
      return picks.front().ticker;
    }
    const auto markets = rest.get_markets();
    if (!markets.empty()) {
      return markets.front().ticker;
    }
    return {};
  }

  DemoCreds creds_;
};

bool level_price_in_range(const std::optional<kalshi::Level> &level) {
  return !level.has_value() ||
         (level->price_cents > 0 && level->price_cents < kalshi::kPriceBasis);
}

bool book_prices_in_range(const kalshi::LocalOrderbook &book) {
  return level_price_in_range(book.best_bid()) &&
         level_price_in_range(book.best_ask());
}

} // namespace

TEST_F(DemoConformanceTest, GetMarketsParses) {
  auto rest = make_rest();
  const auto markets = rest.get_markets();
  ASSERT_FALSE(markets.empty());
  EXPECT_FALSE(markets.front().ticker.empty());
}

TEST_F(DemoConformanceTest, GetOrderbookParsesIntoSaneBbo) {
  const std::string ticker = first_target_ticker();
  ASSERT_FALSE(ticker.empty())
      << "no market found on the demo exchange — there must always be one";
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

TEST_F(DemoConformanceTest, AmendAndDecreaseSemanticsPinned) {
  auto rest = make_rest();
  const auto markets = rest.get_markets();
  ASSERT_FALSE(markets.empty());
  const auto &ticker = markets.front().ticker;
  constexpr int kFarPrice = 2;
  constexpr int kAmendPrice = 3;

  const auto order = rest.place_order(ticker, kalshi::Side::Yes, kFarPrice,
                                      kalshi::Quantity::from_contracts(2),
                                      kalshi::OrderType::Limit);
  ASSERT_FALSE(order.id.empty());

  const auto amended_id =
      rest.amend_order(order.id, ticker, kalshi::Side::Yes, kAmendPrice,
                       kalshi::Quantity::from_contracts(2));
  ASSERT_TRUE(amended_id.has_value()) << "amend must succeed on a far order";

  const auto remaining =
      rest.decrease_order(*amended_id, kalshi::Quantity::from_contracts(1));
  ASSERT_TRUE(remaining.has_value());
  EXPECT_EQ(*remaining, kalshi::Quantity::from_contracts(1))
      << "decrease keeps the order (same id) at reduced size";

  EXPECT_TRUE(rest.cancel_order(*amended_id));
}

TEST_F(DemoConformanceTest, GetFillsParsesWithSaneFields) {
  auto rest = make_rest();
  std::vector<kalshi::Fill> fills;
  ASSERT_NO_THROW(fills = rest.get_fills());
  for (const auto &fill : fills) {
    EXPECT_FALSE(fill.trade_id.empty());
    EXPECT_FALSE(fill.market_ticker.empty());
    EXPECT_GE(fill.price_cents, 1);
    EXPECT_LE(fill.price_cents, 99);
    EXPECT_TRUE(fill.quantity.is_positive());
  }
}

TEST_F(DemoConformanceTest, GetIncentiveProgramsParses) {
  auto rest = make_rest();
  std::vector<kalshi::IncentiveProgram> programs;
  ASSERT_NO_THROW(programs = rest.get_incentive_programs());
  for (const auto &program : programs) {
    EXPECT_FALSE(program.market_ticker.empty());
    EXPECT_GE(program.period_reward_centicents, 0);
  }
}

TEST_F(DemoConformanceTest, CreateOrderResponseParsesRestsAndCancels) {
  const std::string ticker = first_target_ticker();
  ASSERT_FALSE(ticker.empty())
      << "no market found on the demo exchange — there must always be one";
  auto rest = make_rest();

  constexpr int kDeepPassiveBidCents = 1;
  constexpr int kOneContract = 1;

  const auto order =
      rest.place_order(ticker, kalshi::Side::Yes, kDeepPassiveBidCents,
                       kOneContract, kalshi::OrderType::Limit);

  ASSERT_FALSE(order.id.empty());
  EXPECT_EQ(order.filled_quantity, kalshi::Quantity{});
  EXPECT_EQ(order.status, kalshi::OrderStatus::Open);
  EXPECT_NE(order.created_at, std::chrono::system_clock::time_point{});
  EXPECT_EQ(order.price_cents, kDeepPassiveBidCents);
  EXPECT_EQ(order.quantity, kalshi::Quantity::from_contracts(kOneContract));

  bool is_resting = false;
  constexpr int kMaxPollAttempts = 20;
  for (int attempt = 0; attempt < kMaxPollAttempts && !is_resting; ++attempt) {
    const auto resting = rest.get_open_orders();
    is_resting = std::any_of(
        resting.begin(), resting.end(),
        [&order](const kalshi::Order &open) { return open.id == order.id; });
    if (!is_resting) {
      std::this_thread::sleep_for(kOpenOrderPollInterval);
    }
  }
  EXPECT_TRUE(is_resting) << "placed order " << order.id
                          << " not visible in get_open_orders after polling";

  EXPECT_TRUE(rest.cancel_order(order.id));
}

TEST_F(DemoConformanceTest, WebSocketOrderbookSnapshotParses) {
  const std::string ticker = first_target_ticker();
  ASSERT_FALSE(ticker.empty())
      << "no market found on the demo exchange — there must always be one";

  auto websocket = make_ws();

  std::atomic<bool> got_snapshot{false};
  websocket.on_orderbook_snapshot(
      [&got_snapshot](const kalshi::Orderbook &snapshot) {
        if (!snapshot.ticker.empty()) {
          got_snapshot.store(true);
        }
      });
  websocket.subscribe(ticker);

  std::thread runner{[&websocket] { websocket.run(); }};

  constexpr int kMaxWaitTenthsOfSecond = 100;
  for (int tenth = 0; tenth < kMaxWaitTenthsOfSecond && !got_snapshot.load();
       ++tenth) {
    std::this_thread::sleep_for(kEventPollInterval);
  }
  websocket.stop();
  runner.join();

  EXPECT_TRUE(got_snapshot.load())
      << "no orderbook_snapshot received within 10s for ticker " << ticker;
}

TEST_F(DemoConformanceTest, WebSocketDeltaAppliesAndPricesStayValid) {
  const std::string ticker = first_target_ticker();
  ASSERT_FALSE(ticker.empty())
      << "no market found on the demo exchange — there must always be one";

  auto rest = make_rest();
  auto websocket = make_ws();

  std::mutex book_mutex;
  kalshi::LocalOrderbook book;
  std::atomic<bool> got_snapshot{false};
  std::atomic<bool> prices_stayed_valid{true};
  std::atomic<int> delta_count{0};
  std::atomic<bool> saw_induced_price{false};

  constexpr int kInducedBidCents = 3;

  websocket.on_orderbook_snapshot([&](const kalshi::Orderbook &snapshot) {
    const std::lock_guard<std::mutex> lock{book_mutex};
    book.apply_snapshot(snapshot);
    if (!book_prices_in_range(book)) {
      prices_stayed_valid.store(false);
    }
    got_snapshot.store(true);
  });
  websocket.on_orderbook_delta([&](const std::string & /*delta_ticker*/,
                                   kalshi::Side side, int price,
                                   kalshi::Quantity delta) {
    const std::lock_guard<std::mutex> lock{book_mutex};
    book.apply_delta(side, price, delta);
    if (!book_prices_in_range(book)) {
      prices_stayed_valid.store(false);
    }
    delta_count.fetch_add(1);
    if (side == kalshi::Side::Yes && price == kInducedBidCents) {
      saw_induced_price.store(true);
    }
  });
  websocket.subscribe(ticker);

  std::thread runner{[&websocket] { websocket.run(); }};

  constexpr int kMaxWaitTenths = 100;
  for (int tenth = 0; tenth < kMaxWaitTenths && !got_snapshot.load(); ++tenth) {
    std::this_thread::sleep_for(kEventPollInterval);
  }
  ASSERT_TRUE(got_snapshot.load())
      << "no orderbook_snapshot received within 10s for ticker " << ticker;

  constexpr int kOneContract = 1;
  const auto order =
      rest.place_order(ticker, kalshi::Side::Yes, kInducedBidCents,
                       kOneContract, kalshi::OrderType::Limit);
  ASSERT_FALSE(order.id.empty());

  for (int tenth = 0; tenth < kMaxWaitTenths && !saw_induced_price.load();
       ++tenth) {
    std::this_thread::sleep_for(kEventPollInterval);
  }

  EXPECT_TRUE(rest.cancel_order(order.id));

  constexpr int kDrainTenths = 20;
  for (int tenth = 0; tenth < kDrainTenths; ++tenth) {
    std::this_thread::sleep_for(kEventPollInterval);
  }

  websocket.stop();
  runner.join();

  EXPECT_GT(delta_count.load(), 0)
      << "no orderbook_delta received for ticker " << ticker;
  EXPECT_TRUE(saw_induced_price.load())
      << "no delta observed at our induced bid price " << kInducedBidCents;
  EXPECT_TRUE(prices_stayed_valid.load())
      << "a delta produced an out-of-range price";
}

TEST_F(DemoConformanceTest, PlaceNoSideOrderRestsAndCancels) {
  const std::string ticker = first_target_ticker();
  ASSERT_FALSE(ticker.empty())
      << "no market found on the demo exchange — there must always be one";
  auto rest = make_rest();

  constexpr int kDeepPassiveNoBidCents = 1;
  constexpr int kOneContract = 1;

  const auto order =
      rest.place_order(ticker, kalshi::Side::No, kDeepPassiveNoBidCents,
                       kOneContract, kalshi::OrderType::Limit);

  ASSERT_FALSE(order.id.empty());
  EXPECT_EQ(order.status, kalshi::OrderStatus::Open);
  EXPECT_EQ(order.filled_quantity, kalshi::Quantity{});

  bool is_resting = false;
  constexpr int kMaxPollAttempts = 20;
  for (int attempt = 0; attempt < kMaxPollAttempts && !is_resting; ++attempt) {
    const auto resting = rest.get_open_orders();
    is_resting = std::any_of(
        resting.begin(), resting.end(),
        [&order](const kalshi::Order &open) { return open.id == order.id; });
    if (!is_resting) {
      std::this_thread::sleep_for(kOpenOrderPollInterval);
    }
  }
  EXPECT_TRUE(is_resting) << "placed NO order " << order.id
                          << " not visible in get_open_orders after polling";

  EXPECT_TRUE(rest.cancel_order(order.id));
}

TEST_F(DemoConformanceTest, GetMarketsFilteredByEventParses) {
  auto rest = make_rest();
  const auto all_markets = rest.get_markets();
  ASSERT_FALSE(all_markets.empty());

  const std::string &sample = all_markets.front().ticker;
  const auto last_dash = sample.rfind('-');
  if (last_dash == std::string::npos || last_dash == 0) {
    FAIL() << "cannot derive an event ticker from market " << sample;
  }
  const std::string event_ticker = sample.substr(0, last_dash);

  const auto filtered = rest.get_markets(event_ticker);
  if (filtered.empty()) {
    FAIL() << "event filter " << event_ticker << " returned no markets";
  }
  for (const auto &market : filtered) {
    EXPECT_EQ(market.ticker.rfind(event_ticker, 0), 0U)
        << "market " << market.ticker << " does not belong to event "
        << event_ticker;
  }
}
