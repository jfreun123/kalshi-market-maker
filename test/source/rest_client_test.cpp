#include "fake_transport.hpp"
#include "rest_client.hpp"

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <memory>
#include <string>

// ---- Test helpers ----

namespace {

constexpr int kRsaKeyBits = 2048;
constexpr std::string_view kTestBaseUrl = "https://test.kalshi.co/trade-api/v2";
constexpr std::string_view kTestTicker = "KXBTCD";
constexpr int kTestYesPrice = 52;
constexpr int kTestNoPrice = 48;
constexpr int kTestQuantity = 10;
constexpr int kTestFilledQty = 2;
constexpr int kTestFeeRateBps = 7;
constexpr std::size_t kOneResult = 1U;
constexpr std::size_t kTwoResults = 2U;
constexpr int kHttpOk = 200;
constexpr int kHttpUnauthorized = 401;
constexpr int kHttpNotFound = 404;
constexpr int kHttpInternalError = 500;

// V2 minimal create-order response.
constexpr std::string_view kPlaceOrderResponse =
    R"({"order_id":"order-abc","fill_count":"0.00","remaining_count":"10.00","ts_ms":1718000000000})";

// V2 GET order with a partial fill (fill_count_fp > 0, still resting).
constexpr std::string_view kPartialOrderJson =
    R"({"order_id":"order-abc","ticker":"KXBTCD","outcome_side":"yes","type":"limit","yes_price_dollars":"0.5200","no_price_dollars":"0.4800","initial_count_fp":"10.00","fill_count_fp":"2.00","status":"resting","created_time":"2025-01-01T00:00:00Z"})";

std::string generate_test_private_key() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C API
  EVP_PKEY *pkey = EVP_RSA_gen(static_cast<unsigned int>(kRsaKeyBits));
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  BUF_MEM *buf_mem = nullptr;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C macro
  BIO_get_mem_ptr(bio, &buf_mem);
  std::string result(buf_mem->data, buf_mem->length);
  BIO_free(bio);
  EVP_PKEY_free(pkey);
  return result;
}

} // namespace

// ---- Test fixture ----

class RestClientTest : public ::testing::Test {
  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes):
  // GTest requires protected members accessible in TEST_F bodies.
protected:
  static void SetUpTestSuite() { s_priv_pem_ = generate_test_private_key(); }

  [[nodiscard]] static kalshi::RestClient
  make_client(std::unique_ptr<FakeTransport> transport) {
    return kalshi::RestClient{kalshi::Auth{"test-api-key", s_priv_pem_},
                              std::move(transport), std::string(kTestBaseUrl)};
  }

  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  static std::string s_priv_pem_; // NOLINT — GTest fixture static
};

std::string RestClientTest::s_priv_pem_; // NOLINT — GTest fixture static

// ---- get_markets ----

TEST_F(RestClientTest, GetMarketsCallsGetWithMarketsUrl) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"markets":[],"cursor":""})"});
  auto client = make_client(std::move(transport));

  client.get_markets();

  EXPECT_EQ(transport_raw->last_request().method, "GET");
  EXPECT_NE(transport_raw->last_request().url.find("/markets"),
            std::string::npos);
}

TEST_F(RestClientTest, GetMarketsParsesSingleMarket) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  // fee_rate_bps is no longer in the Kalshi API schema but we keep it in the
  // fixture to verify value_or() still reads it when present.
  transport_raw->enqueue(
      {kHttpOk,
       R"({"markets":[{"ticker":"KXBTCD","title":"Bitcoin above $30k?","close_time":"2025-12-25T00:00:00Z","fee_rate_bps":7}],"cursor":""})"});
  auto client = make_client(std::move(transport));

  auto markets = client.get_markets();

  ASSERT_EQ(markets.size(), kOneResult);
  EXPECT_EQ(markets[0].ticker, "KXBTCD");
  EXPECT_EQ(markets[0].title, "Bitcoin above $30k?");
  EXPECT_EQ(markets[0].fee_rate_bps, kTestFeeRateBps);
  EXPECT_NE(markets[0].close_time,
            std::chrono::system_clock::time_point{}); // non-default
}

// ---- get_incentive_programs ----

TEST_F(RestClientTest, GetIncentiveProgramsParses) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk, R"({"incentive_programs":[{"market_ticker":"KXNBA-LAL",)"
                R"("period_reward":650000,"target_size_fp":"1000.00",)"
                R"("discount_factor_bps":5000}],"next_cursor":""})"});
  auto client = make_client(std::move(transport));

  const auto programs = client.get_incentive_programs();

  ASSERT_EQ(programs.size(), kOneResult);
  EXPECT_EQ(programs.front().market_ticker, "KXNBA-LAL");
  EXPECT_EQ(programs.front().period_reward_centicents, 650000);
  EXPECT_EQ(programs.front().target_size,
            kalshi::Quantity::from_contracts(1000));
  EXPECT_EQ(programs.front().discount_factor_bps, 5000);
}

TEST_F(RestClientTest, GetIncentiveProgramsToleratesNullOptionalFields) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"incentive_programs":[{"market_ticker":"KXFOO","period_reward":50000,)"
       R"("target_size_fp":null,"discount_factor_bps":null}]})"});
  auto client = make_client(std::move(transport));

  const auto programs = client.get_incentive_programs();

  ASSERT_EQ(programs.size(), kOneResult);
  EXPECT_EQ(programs.front().period_reward_centicents, 50000);
  EXPECT_EQ(programs.front().target_size, kalshi::Quantity{});
  EXPECT_EQ(programs.front().discount_factor_bps, 0);
}

TEST_F(RestClientTest, GetIncentiveProgramsEmptyBodyYieldsNoPrograms) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({})"});
  auto client = make_client(std::move(transport));

  EXPECT_TRUE(client.get_incentive_programs().empty());
}

TEST_F(RestClientTest, GetMarketsParsesDailyVolume) {
  // The scanner ranks on 24h volume (live flow), not lifetime volume_fp.
  constexpr double kExpectedDailyVolume = 6024216.16;
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"markets":[{"ticker":"KXBTCD","close_time":"2025-12-25T00:00:00Z",)"
       R"("volume_fp":"15929810.28","volume_24h_fp":"6024216.16"}],"cursor":""})"});
  auto client = make_client(std::move(transport));

  auto markets = client.get_markets();

  ASSERT_EQ(markets.size(), kOneResult);
  EXPECT_DOUBLE_EQ(markets[0].volume_24h, kExpectedDailyVolume);
}

TEST_F(RestClientTest, GetMarketsWithMissingFeeRateBpsDefaultsToZero) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"markets":[{"ticker":"KXBTCD","title":"Test","close_time":"2025-12-25T00:00:00Z"}],"cursor":""})"});
  auto client = make_client(std::move(transport));

  auto markets = client.get_markets();

  ASSERT_EQ(markets.size(), kOneResult);
  EXPECT_EQ(markets[0].fee_rate_bps, 0);
}

TEST_F(RestClientTest, GetMarketsReturnsEmptyVectorForEmptyList) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"markets":[],"cursor":""})"});
  auto client = make_client(std::move(transport));

  auto markets = client.get_markets();

  EXPECT_TRUE(markets.empty());
}

TEST_F(RestClientTest, GetMarketsWithEventTickerAppendsQueryParam) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"markets":[],"cursor":""})"});
  auto client = make_client(std::move(transport));

  client.get_markets("KXBTCD");

  EXPECT_NE(transport_raw->last_request().url.find("event_ticker=KXBTCD"),
            std::string::npos);
}

TEST_F(RestClientTest, GetMarketsRequestsLargePageSize) {
  // A large limit minimizes round-trips when paginating the full listing.
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"markets":[],"cursor":""})"});
  auto client = make_client(std::move(transport));

  client.get_markets();

  EXPECT_NE(transport_raw->last_request().url.find("limit=1000"),
            std::string::npos);
}

TEST_F(RestClientTest, GetMarketsRequestsOnlyOpenMarkets) {
  // Settled/closed markets are never tradeable; restrict the listing to open
  // markets to avoid paging through the full historical archive.
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"markets":[],"cursor":""})"});
  auto client = make_client(std::move(transport));

  client.get_markets();

  EXPECT_NE(transport_raw->last_request().url.find("status=open"),
            std::string::npos);
}

TEST_F(RestClientTest, GetMarketsFollowsCursorToFetchAllPages) {
  // First page returns one market and a non-empty cursor; second page returns
  // a second market and an empty cursor, signalling end of results.
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"markets":[{"ticker":"KXBTCD","title":"Page1","close_time":"2025-12-25T00:00:00Z"}],"cursor":"page2token"})"});
  transport_raw->enqueue(
      {kHttpOk,
       R"({"markets":[{"ticker":"KXETH","title":"Page2","close_time":"2025-12-25T00:00:00Z"}],"cursor":""})"});
  auto client = make_client(std::move(transport));

  auto markets = client.get_markets();

  ASSERT_EQ(markets.size(), kTwoResults);
  EXPECT_EQ(markets[0].ticker, "KXBTCD");
  EXPECT_EQ(markets[1].ticker, "KXETH");
}

TEST_F(RestClientTest, GetMarketsThrowsOnHttpError) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpInternalError, R"({"error":"internal server error"})"});
  auto client = make_client(std::move(transport));

  EXPECT_THROW(client.get_markets(), std::runtime_error);
}

// ---- get_positions ----

TEST_F(RestClientTest, GetPositionsParsesMarketPositionFields) {
  // Schema mirrors the live GET /portfolio/positions response: position_fp is a
  // signed fixed-point count; money fields are *_dollars fixed-point strings.
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"market_positions":[{"ticker":"KXFED-26SEP-T3.00","position_fp":"-7.00",)"
       R"("realized_pnl_dollars":"1.250000","market_exposure_dollars":"3.080000",)"
       R"("resting_orders_count":2}],"cursor":""})"});
  auto client = make_client(std::move(transport));

  auto positions = client.get_positions();

  ASSERT_EQ(positions.size(), kOneResult);
  EXPECT_EQ(positions[0].ticker, "KXFED-26SEP-T3.00");
  EXPECT_EQ(positions[0].position,
            kalshi::Quantity::from_contracts(-7)); // NO position -> negative
  EXPECT_DOUBLE_EQ(positions[0].realized_pnl_cents, 125.0);
  EXPECT_DOUBLE_EQ(positions[0].market_exposure_cents, 308.0);
  EXPECT_EQ(positions[0].resting_orders_count, 2);
}

TEST_F(RestClientTest, GetPositionsFollowsCursorAcrossPages) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"market_positions":[{"ticker":"KXA","position_fp":"5.00"}],"cursor":"next"})"});
  transport_raw->enqueue(
      {kHttpOk,
       R"({"market_positions":[{"ticker":"KXB","position_fp":"3.00"}],"cursor":""})"});
  auto client = make_client(std::move(transport));

  auto positions = client.get_positions();

  ASSERT_EQ(positions.size(), kTwoResults);
  EXPECT_EQ(positions[0].ticker, "KXA");
  EXPECT_EQ(positions[1].ticker, "KXB");
}

TEST_F(RestClientTest, GetPositionsRequestsPositionsEndpoint) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"market_positions":[],"cursor":""})"});
  auto client = make_client(std::move(transport));

  client.get_positions();

  EXPECT_NE(transport_raw->last_request().url.find("/portfolio/positions"),
            std::string::npos);
}

// ---- get_orderbook ----

TEST_F(RestClientTest, GetOrderbookCallsCorrectUrl) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"orderbook_fp":{"yes_dollars":[["0.5200","100.00"]],"no_dollars":[["0.4800","150.00"]]}})"});
  auto client = make_client(std::move(transport));

  client.get_orderbook(kTestTicker);

  const auto &request_url = transport_raw->last_request().url;
  EXPECT_NE(request_url.find("KXBTCD"), std::string::npos);
  EXPECT_NE(request_url.find("orderbook"), std::string::npos);
}

TEST_F(RestClientTest, GetOrderbookParsesYesAndNoLevels) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk,
       R"({"orderbook_fp":{"yes_dollars":[["0.5500","200.00"],["0.5200","100.00"]],"no_dollars":[["0.4500","150.00"],["0.4300","300.00"]]}})"});
  auto client = make_client(std::move(transport));

  auto orderbook = client.get_orderbook(kTestTicker);

  ASSERT_EQ(orderbook.yes.size(), kTwoResults);
  EXPECT_EQ(orderbook.yes[0].price_cents, 55);
  EXPECT_EQ(orderbook.yes[0].quantity, kalshi::Quantity::from_contracts(200));
  EXPECT_EQ(orderbook.yes[1].price_cents, 52);

  ASSERT_EQ(orderbook.no.size(), kTwoResults);
  EXPECT_EQ(orderbook.no[0].price_cents, 45);
  EXPECT_EQ(orderbook.no[0].quantity, kalshi::Quantity::from_contracts(150));
}

TEST_F(RestClientTest, GetOrderbookSetsTicker) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk, R"({"orderbook_fp":{"yes_dollars":[],"no_dollars":[]}})"});
  auto client = make_client(std::move(transport));

  auto orderbook = client.get_orderbook(kTestTicker);

  EXPECT_EQ(orderbook.ticker, std::string(kTestTicker));
}

TEST_F(RestClientTest, GetOrderbookThrowsOnHttpError) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpNotFound, R"({"error":"market not found"})"});
  auto client = make_client(std::move(transport));

  EXPECT_THROW(client.get_orderbook(kTestTicker), std::runtime_error);
}

// ---- place_order ----

TEST_F(RestClientTest, PlaceOrderSendsPostToEventsOrdersUrl) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, std::string(kPlaceOrderResponse)});
  auto client = make_client(std::move(transport));

  client.place_order(kTestTicker, kalshi::Side::Yes, kTestYesPrice,
                     kTestQuantity, kalshi::OrderType::Limit);

  EXPECT_EQ(transport_raw->last_request().method, "POST");
  EXPECT_NE(transport_raw->last_request().url.find("/portfolio/events/orders"),
            std::string::npos);
}

TEST_F(RestClientTest, PlaceOrderYesSideUsesBidWithYesPrice) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, std::string(kPlaceOrderResponse)});
  auto client = make_client(std::move(transport));

  client.place_order(kTestTicker, kalshi::Side::Yes, kTestYesPrice,
                     kTestQuantity, kalshi::OrderType::Limit);

  const auto &body = transport_raw->last_request().body;
  EXPECT_NE(body.find(R"("side":"bid")"), std::string::npos);
  EXPECT_NE(body.find(R"("price":"0.5200")"), std::string::npos);
}

TEST_F(RestClientTest, PlaceOrderNoSideUsesAskWithComplementPrice) {
  // Buying NO at 48 cents = selling YES at 52 cents (the YES complement).
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, std::string(kPlaceOrderResponse)});
  auto client = make_client(std::move(transport));

  client.place_order(kTestTicker, kalshi::Side::No, kTestNoPrice, kTestQuantity,
                     kalshi::OrderType::Limit);

  const auto &body = transport_raw->last_request().body;
  EXPECT_NE(body.find(R"("side":"ask")"), std::string::npos);
  // YES price = 100 - 48 = 52 cents = "0.5200"
  EXPECT_NE(body.find(R"("price":"0.5200")"), std::string::npos);
}

TEST_F(RestClientTest, FlattenLongYesSellsYesWithFractionalIocOrder) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, std::string(kPlaceOrderResponse)});
  auto client = make_client(std::move(transport));

  client.flatten(kTestTicker, kalshi::Quantity::from_fp_string("4.36"));

  const auto &body = transport_raw->last_request().body;
  EXPECT_NE(body.find(R"("side":"ask")"), std::string::npos);
  EXPECT_NE(body.find(R"("count":"4.36")"), std::string::npos);
  EXPECT_NE(body.find(R"("time_in_force":"immediate_or_cancel")"),
            std::string::npos);
}

TEST_F(RestClientTest, FlattenLongNoBuysYes) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, std::string(kPlaceOrderResponse)});
  auto client = make_client(std::move(transport));

  client.flatten(kTestTicker, -kalshi::Quantity::from_fp_string("22.41"));

  const auto &body = transport_raw->last_request().body;
  EXPECT_NE(body.find(R"("side":"bid")"), std::string::npos);
  EXPECT_NE(body.find(R"("count":"22.41")"), std::string::npos);
  EXPECT_NE(body.find(R"("time_in_force":"immediate_or_cancel")"),
            std::string::npos);
}

TEST_F(RestClientTest, FlattenFlatPositionPlacesNoOrder) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  auto client = make_client(std::move(transport));

  client.flatten(kTestTicker, kalshi::Quantity{});

  EXPECT_EQ(transport_raw->recorded_requests().size(), 0U);
}

TEST_F(RestClientTest, PlaceOrderParsesResponseIntoOrder) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, std::string(kPlaceOrderResponse)});
  auto client = make_client(std::move(transport));

  auto order = client.place_order(kTestTicker, kalshi::Side::Yes, kTestYesPrice,
                                  kTestQuantity, kalshi::OrderType::Limit);

  EXPECT_EQ(order.id, "order-abc");
  EXPECT_EQ(order.market_ticker, std::string(kTestTicker));
  EXPECT_EQ(order.side, kalshi::Side::Yes);
  EXPECT_EQ(order.price_cents, kTestYesPrice);
  EXPECT_EQ(order.quantity, kalshi::Quantity::from_contracts(kTestQuantity));
  EXPECT_EQ(order.filled_quantity, kalshi::Quantity{});
  EXPECT_EQ(order.status, kalshi::OrderStatus::Open);
  EXPECT_EQ(order.type, kalshi::OrderType::Limit);
  EXPECT_NE(order.created_at, std::chrono::system_clock::time_point{});
}

TEST_F(RestClientTest, IocOrderUnfilledRemainderIsCancelledNotOpen) {
  const std::string unfilled_ioc_response =
      R"({"order_id":"order-abc","fill_count":"0.00","remaining_count":"0.00","ts_ms":1718000000000})";
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, unfilled_ioc_response});
  auto client = make_client(std::move(transport));

  auto order = client.place_order(kTestTicker, kalshi::Side::Yes, kTestYesPrice,
                                  kTestQuantity, kalshi::OrderType::Market);

  EXPECT_EQ(order.status, kalshi::OrderStatus::Cancelled)
      << "an IOC never rests: its unfilled remainder is cancelled by the "
         "matching engine";
}

TEST_F(RestClientTest, IocOrderPartialFillRemainderIsCancelledNotOpen) {
  constexpr int kPartialFill = 3;
  const std::string partial_ioc_response =
      R"({"order_id":"order-abc","fill_count":"3.00","remaining_count":"0.00","ts_ms":1718000000000})";
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, partial_ioc_response});
  auto client = make_client(std::move(transport));

  auto order = client.place_order(kTestTicker, kalshi::Side::Yes, kTestYesPrice,
                                  kTestQuantity, kalshi::OrderType::Market);

  EXPECT_EQ(order.filled_quantity,
            kalshi::Quantity::from_contracts(kPartialFill));
  EXPECT_EQ(order.status, kalshi::OrderStatus::Cancelled);
}

TEST_F(RestClientTest, PlaceOrderParsesAverageFillPriceSideNative) {
  constexpr int kAvgYesCents = 56;
  const std::string filled_response =
      R"({"order_id":"order-abc","fill_count":"10.00","remaining_count":"0.00",)"
      R"("average_fill_price":"0.5600","ts_ms":1718000000000})";
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, filled_response});
  auto client = make_client(std::move(transport));

  auto order = client.place_order(kTestTicker, kalshi::Side::No, kTestYesPrice,
                                  kTestQuantity, kalshi::OrderType::Market);

  EXPECT_EQ(order.average_fill_price_cents, kalshi::kPriceBasis - kAvgYesCents)
      << "average_fill_price is the YES leg; a NO order books its complement";
}

TEST_F(RestClientTest, PlaceOrderWithoutAverageFillPriceLeavesZero) {
  const std::string unfilled_response =
      R"({"order_id":"order-abc","fill_count":"0.00","remaining_count":"10.00","ts_ms":1718000000000})";
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, unfilled_response});
  auto client = make_client(std::move(transport));

  auto order = client.place_order(kTestTicker, kalshi::Side::Yes, kTestYesPrice,
                                  kTestQuantity, kalshi::OrderType::Limit);

  EXPECT_EQ(order.average_fill_price_cents, 0);
}

TEST_F(RestClientTest, PlaceOrderParsesImmediateFillCountFromResponse) {
  constexpr int kImmediateFill = 3;
  const std::string partial_fill_response =
      R"({"order_id":"order-abc","fill_count":"3.00","remaining_count":"7.00","ts_ms":1718000000000})";
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, partial_fill_response});
  auto client = make_client(std::move(transport));

  auto order = client.place_order(kTestTicker, kalshi::Side::Yes, kTestYesPrice,
                                  kTestQuantity, kalshi::OrderType::Limit);

  EXPECT_EQ(order.filled_quantity,
            kalshi::Quantity::from_contracts(kImmediateFill));
  EXPECT_EQ(order.status, kalshi::OrderStatus::PartiallyFilled);
}

// ---- cancel_order ----

TEST_F(RestClientTest, CancelOrderSendsDeleteToCorrectUrl) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk, R"({"order":{"order_id":"order-abc","status":"canceled"}})"});
  auto client = make_client(std::move(transport));

  client.cancel_order("order-abc");

  EXPECT_EQ(transport_raw->last_request().method, "DELETE");
  EXPECT_NE(transport_raw->last_request().url.find("order-abc"),
            std::string::npos);
}

TEST_F(RestClientTest, CancelOrderReturnsTrueOn200) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk, R"({"order":{"order_id":"order-abc","status":"canceled"}})"});
  auto client = make_client(std::move(transport));

  EXPECT_TRUE(client.cancel_order("order-abc"));
}

TEST_F(RestClientTest, CancelOrderReturnsFalseOn404) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpNotFound, R"({"error":"order not found"})"});
  auto client = make_client(std::move(transport));

  EXPECT_FALSE(client.cancel_order("nonexistent-order"));
}

// ---- get_open_orders ----

TEST_F(RestClientTest, GetOpenOrdersCallsGetWithOrdersUrl) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"orders":[],"cursor":""})"});
  auto client = make_client(std::move(transport));

  client.get_open_orders();

  EXPECT_EQ(transport_raw->last_request().method, "GET");
  EXPECT_NE(transport_raw->last_request().url.find("/portfolio/orders"),
            std::string::npos);
}

TEST_F(RestClientTest, GetOpenOrdersParsesOrdersList) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"orders":[)" +
                                       std::string(kPartialOrderJson) +
                                       R"(],"cursor":""})"});
  auto client = make_client(std::move(transport));

  auto orders = client.get_open_orders();

  ASSERT_EQ(orders.size(), kOneResult);
  EXPECT_EQ(orders[0].id, "order-abc");
  EXPECT_EQ(orders[0].market_ticker, "KXBTCD");
  EXPECT_EQ(orders[0].price_cents, kTestYesPrice);
  EXPECT_EQ(orders[0].filled_quantity,
            kalshi::Quantity::from_contracts(kTestFilledQty));
}

TEST_F(RestClientTest, ParsesFractionalSecondTimestamp) {
  const std::string order_with_micros =
      R"({"order_id":"order-abc","ticker":"KXBTCD","outcome_side":"yes",)"
      R"("type":"limit","yes_price_dollars":"0.5200","no_price_dollars":"0.4800",)"
      R"("initial_count_fp":"10.00","fill_count_fp":"0.00","status":"resting",)"
      R"("created_time":"2026-07-03T00:00:11.556415Z"})";
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk, R"({"orders":[)" + order_with_micros + R"(],"cursor":""})"});
  auto client = make_client(std::move(transport));

  std::vector<kalshi::Order> orders;
  ASSERT_NO_THROW(orders = client.get_open_orders());
  ASSERT_EQ(orders.size(), kOneResult);
  EXPECT_NE(orders[0].created_at, std::chrono::system_clock::time_point{});
}

TEST_F(RestClientTest, GetOpenOrdersThrowsOnHttpError) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpUnauthorized, R"({"error":"unauthorized"})"});
  auto client = make_client(std::move(transport));

  EXPECT_THROW(client.get_open_orders(), std::runtime_error);
}

TEST_F(RestClientTest, MeasureClockSkewReadsDateHeader) {
  const std::string server_date = "Fri, 03 Jul 2026 19:37:09 GMT";
  constexpr long long kServerEpochSeconds = 1'783'107'429LL;
  constexpr auto kLocalAhead = std::chrono::seconds{760};
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue(
      {kHttpOk, R"({"exchange_active":true})", {{"Date", server_date}}});
  auto client = make_client(std::move(transport));
  const auto local_now =
      kalshi::SystemTimePoint{std::chrono::seconds{kServerEpochSeconds}} +
      kLocalAhead;

  const auto skew = client.measure_clock_skew(local_now);

  ASSERT_TRUE(skew.has_value());
  EXPECT_EQ(*skew, kLocalAhead);
  EXPECT_NE(transport_raw->last_request().url.find("/exchange/status"),
            std::string::npos);
}

TEST_F(RestClientTest, MeasureClockSkewWorksEvenWhenRequestIsRejected) {
  const std::string server_date = "Fri, 03 Jul 2026 19:37:09 GMT";
  constexpr long long kServerEpochSeconds = 1'783'107'429LL;
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpUnauthorized,
                          R"({"error":"header_timestamp_expired"})",
                          {{"Date", server_date}}});
  auto client = make_client(std::move(transport));
  const auto local_now =
      kalshi::SystemTimePoint{std::chrono::seconds{kServerEpochSeconds}};

  const auto skew = client.measure_clock_skew(local_now);

  ASSERT_TRUE(skew.has_value());
  EXPECT_EQ(skew->count(), 0);
}

TEST_F(RestClientTest, MeasureClockSkewEmptyWithoutDateHeader) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport *const transport_raw = transport.get();
  transport_raw->enqueue({kHttpOk, R"({"exchange_active":true})"});
  auto client = make_client(std::move(transport));

  EXPECT_FALSE(client.measure_clock_skew().has_value());
}
