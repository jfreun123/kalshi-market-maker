#include "fake_transport.hpp"
#include "order_manager.hpp"
#include "rest_client.hpp"

#include <gtest/gtest.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

// ---- Test constants ----

namespace {

constexpr int kContractCents = 100;
constexpr int kYesBidPrice = 52;
constexpr int kNoBidPrice = 44;
constexpr int kOrderQty = 10;
constexpr int kFillQty = 5;
constexpr int kFullFillQty = 10;
constexpr int kSecondFillQty = 5;
constexpr int kYesFillPrice = 52;
constexpr int kNoFillPrice = 44;
constexpr int kSecondNoFillPrice = 46;
constexpr int kContractPayout = 100; // a winning binary contract pays 100c
constexpr int kNoMatchQty = 3; // partial offset against a 5-lot YES position
// Each YES@52 + NO@44 pair: PnL = 100 - 52 - 44 = 4 cents per contract
constexpr double kExpectedPnlPerPair = 4.0;
constexpr double kExpectedPnl5Pairs = 20.0;
constexpr double kExpectedPnlMixed = 30.0;
constexpr std::size_t kOneOrder = 1U;
constexpr long long kTs1Ns = 1'000'000LL;
constexpr long long kTs2Ns = 2'000'000LL;
constexpr long long kTs3Ns = 3'000'000LL;
constexpr int kHttpOk = 200;
constexpr int kHttpNotFound = 404;

const std::string kTicker = "KXBTCD";
const std::string kOtherTicker = "KXETHU";
const std::string kOrderId = "order-abc123";
const std::string kOrderId2 = "order-def456";
const std::string kApiKey = "test-key-id";
const std::string kBaseUrl = "https://trading-api.kalshi.com/trade-api/v2";

// RSA key generated once per test suite.
std::string
    kPemPrivateKey; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

std::string generate_rsa_pem() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  EVP_PKEY *pkey = EVP_RSA_gen(2048U);
  if (pkey == nullptr) {
    return "";
  }
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  char *pem_data = nullptr;
  long pem_len = BIO_get_mem_data(bio, &pem_data); // NOLINT(runtime/int)
  std::string pem(pem_data, static_cast<std::size_t>(pem_len));
  BIO_free(bio);
  EVP_PKEY_free(pkey);
  return pem;
}

// Builds the V2 minimal response body that RestClient expects for a placed
// order.
std::string order_response_json(const std::string &order_id, int qty) {
  return R"({"order_id":")" + order_id +
         R"(","fill_count_fp":"0.00","remaining_count":")" + std::to_string(qty) +
         R"(.00","ts_ms":1718000000000})";
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
kalshi::Fill make_fill(const std::string &order_id, const std::string &ticker,
                       kalshi::Side side, int price_cents, int quantity,
                       long long timestamp_ns = kTs1Ns) {
  kalshi::Fill fill;
  fill.order_id = order_id;
  fill.market_ticker = ticker;
  fill.side = side;
  fill.price_cents = price_cents;
  fill.quantity = quantity;
  fill.timestamp = std::chrono::system_clock::time_point{
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::nanoseconds{timestamp_ns})};
  return fill;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace

// ---- Test fixture ----

class OrderManagerTest : public ::testing::Test {
public:
  static void SetUpTestSuite() { kPemPrivateKey = generate_rsa_pem(); }
};

// ---- Helpers used within tests ----

namespace {

// Creates a RestClient backed by the given transport.
kalshi::RestClient make_rest_client(std::unique_ptr<FakeTransport> transport) {
  return kalshi::RestClient{kalshi::Auth{kApiKey, kPemPrivateKey},
                            std::move(transport), kBaseUrl};
}

} // namespace

// ---- Initial state ----

TEST_F(OrderManagerTest, OpenOrdersInitiallyEmpty) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  EXPECT_TRUE(mgr.open_orders().empty());
}

TEST_F(OrderManagerTest, NetPositionDefaultsToZero) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  EXPECT_EQ(mgr.net_position(kTicker), 0);
}

TEST_F(OrderManagerTest, RealizedPnlDefaultsToZero) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  EXPECT_DOUBLE_EQ(mgr.realized_pnl(kTicker), 0.0);
}

// ---- place() ----

TEST_F(OrderManagerTest, PlaceAddsOrderToOpenOrders) {
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});

  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty);

  EXPECT_EQ(mgr.open_orders().size(), kOneOrder);
  EXPECT_TRUE(mgr.open_orders().contains(kOrderId));
}

TEST_F(OrderManagerTest, PlaceReturnsOrderWithCorrectTicker) {
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});

  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  const auto order =
      mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty);

  EXPECT_EQ(order.market_ticker, kTicker);
  EXPECT_EQ(order.id, kOrderId);
  EXPECT_EQ(order.price_cents, kYesBidPrice);
  EXPECT_EQ(order.quantity, kOrderQty);
}

// ---- cancel() ----

TEST_F(OrderManagerTest, CancelRemovesOrderFromOpenOrders) {
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});
  transport->enqueue({kHttpOk, "{}"}); // cancel response

  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty);

  const bool cancelled = mgr.cancel(kOrderId);

  EXPECT_TRUE(cancelled);
  EXPECT_TRUE(mgr.open_orders().empty());
}

TEST_F(OrderManagerTest, CancelReturnsFalseWhenApiRejects) {
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});
  transport->enqueue({kHttpNotFound, "{}"}); // cancel rejected

  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty);

  const bool cancelled = mgr.cancel(kOrderId);

  EXPECT_FALSE(cancelled);
  EXPECT_EQ(mgr.open_orders().size(), kOneOrder); // order still present
}

TEST_F(OrderManagerTest, CancelAllCancelsAllOrdersForTicker) {
  auto transport = std::make_unique<FakeTransport>();
  // Place 2 orders on kTicker, 1 on kOtherTicker.
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});
  transport->enqueue({kHttpOk, order_response_json(kOrderId2, kOrderQty)});
  transport->enqueue({kHttpOk, "{}"}); // cancel kTicker order

  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty);
  mgr.place(kOtherTicker, kalshi::Side::No, kNoBidPrice, kOrderQty);

  mgr.cancel_all(kTicker);

  // kTicker order cancelled; kOtherTicker order still open.
  EXPECT_EQ(mgr.open_orders().size(), kOneOrder);
  EXPECT_TRUE(mgr.open_orders().contains(kOrderId2));
}

namespace {
// A transport that throws on the DELETE for one specific order id, so we can
// verify cancel_all is best-effort: a failed cancel must not abort the others.
class ThrowOnCancelTransport : public FakeTransport {
public:
  explicit ThrowOnCancelTransport(std::string doomed_order_id)
      : doomed_order_id_{std::move(doomed_order_id)} {}

  kalshi::HttpResponse
  delete_(std::string_view url,
          const std::map<std::string, std::string> &headers) override {
    if (url.find(doomed_order_id_) != std::string_view::npos) {
      throw std::runtime_error("simulated network failure during cancel");
    }
    return FakeTransport::delete_(url, headers);
  }

private:
  std::string doomed_order_id_;
};
} // namespace

TEST_F(OrderManagerTest, CancelAllIsBestEffortWhenOneCancelThrows) {
  auto transport = std::make_unique<ThrowOnCancelTransport>(kOrderId);
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});
  transport->enqueue({kHttpOk, order_response_json(kOrderId2, kOrderQty)});

  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty); // kOrderId
  mgr.place(kTicker, kalshi::Side::No, kNoBidPrice, kOrderQty);   // kOrderId2

  // Must not throw even though kOrderId's cancel fails.
  EXPECT_NO_THROW(mgr.cancel_all(kTicker));

  // kOrderId's cancel threw (still open); kOrderId2's succeeded (removed).
  EXPECT_EQ(mgr.open_orders().size(), kOneOrder);
  EXPECT_TRUE(mgr.open_orders().contains(kOrderId));
}

// ---- record_fill / net_position ----

TEST_F(OrderManagerTest, RecordYesFillIncreasesNetPosition) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  mgr.record_fill(
      make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice, kFillQty));

  EXPECT_EQ(mgr.net_position(kTicker), kFillQty);
}

TEST_F(OrderManagerTest, RecordNoFillDecreasesNetPosition) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  mgr.record_fill(
      make_fill(kOrderId, kTicker, kalshi::Side::No, kNoFillPrice, kFillQty));

  EXPECT_EQ(mgr.net_position(kTicker), -kFillQty);
}

TEST_F(OrderManagerTest, YesAndNoFillsNetToZeroPosition) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  mgr.record_fill(
      make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice, kFillQty));
  mgr.record_fill(make_fill(kOrderId2, kTicker, kalshi::Side::No, kNoFillPrice,
                            kFillQty, kTs2Ns));

  EXPECT_EQ(mgr.net_position(kTicker), 0);
}

TEST_F(OrderManagerTest, DuplicateFillIsIdempotent) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  const auto fill =
      make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice, kFillQty);
  mgr.record_fill(fill);
  mgr.record_fill(fill); // same fill again

  // Should count only once.
  EXPECT_EQ(mgr.net_position(kTicker), kFillQty);
}

TEST_F(OrderManagerTest, FilledCountUpdatedOnPartialFill) {
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});
  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty);

  mgr.record_fill(
      make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice, kFillQty));

  ASSERT_TRUE(mgr.open_orders().contains(kOrderId));
  const kalshi::Order
      &open_order = // NOLINT(bugprone-unchecked-optional-access)
      mgr.open_orders().at(kOrderId);
  EXPECT_EQ(open_order.filled_quantity, kFillQty);
  EXPECT_EQ(open_order.status, kalshi::OrderStatus::PartiallyFilled);
}

TEST_F(OrderManagerTest, FullyFilledOrderRemovedFromOpenOrders) {
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});
  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty);

  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFullFillQty));

  EXPECT_TRUE(mgr.open_orders().empty());
}

TEST_F(OrderManagerTest, CancelFilledOrderHandledGracefully) {
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId, kOrderQty)});
  transport->enqueue({kHttpOk, "{}"}); // cancel response
  auto rest_client = make_rest_client(std::move(transport));
  kalshi::OrderManager mgr{rest_client};
  mgr.place(kTicker, kalshi::Side::Yes, kYesBidPrice, kOrderQty);

  // Simulate fill arriving before cancel reaches the exchange.
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFullFillQty));
  // Now cancel (the order is already gone locally; REST still succeeds).
  const bool cancelled = mgr.cancel(kOrderId);

  EXPECT_TRUE(cancelled);
  EXPECT_TRUE(mgr.open_orders().empty()); // already removed by fill
}

// ---- realized_pnl ----

TEST_F(OrderManagerTest, RealizedPnlFromMatchedYesAndNoFills) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  // Buy 5 YES @ 52: goes into inventory.
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFillQty, kTs1Ns));
  // Buy 5 NO @ 44: matches 5 YES. PnL = (100 - 52 - 44) * 5 = 20.
  mgr.record_fill(make_fill(kOrderId2, kTicker, kalshi::Side::No, kNoFillPrice,
                            kFillQty, kTs2Ns));

  EXPECT_DOUBLE_EQ(mgr.realized_pnl(kTicker), kExpectedPnlPerPair * kFillQty);
}

TEST_F(OrderManagerTest, RealizedPnlAccumulatesAcrossMultiplePairs) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  // Buy 10 YES @ 52.
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kOrderQty, kTs1Ns));
  // Buy 5 NO @ 44: matches 5 YES. PnL = 4 * 5 = 20.
  mgr.record_fill(make_fill("fill-no-1", kTicker, kalshi::Side::No,
                            kNoFillPrice, kFillQty, kTs2Ns));
  // Buy 5 NO @ 46: matches remaining 5 YES. PnL += 2 * 5 = 10. Total = 30.
  mgr.record_fill(make_fill("fill-no-2", kTicker, kalshi::Side::No,
                            kSecondNoFillPrice, kSecondFillQty, kTs3Ns));

  EXPECT_DOUBLE_EQ(mgr.realized_pnl(kTicker), kExpectedPnlMixed);
}

TEST_F(OrderManagerTest, UnmatchedFillsHaveZeroRealizedPnl) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  // Only YES fills, no NO fills to match against.
  mgr.record_fill(
      make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice, kFillQty));

  EXPECT_DOUBLE_EQ(mgr.realized_pnl(kTicker), 0.0);
}

TEST_F(OrderManagerTest, PnlIsolatedPerTicker) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFillQty, kTs1Ns));
  mgr.record_fill(make_fill(kOrderId2, kTicker, kalshi::Side::No, kNoFillPrice,
                            kFillQty, kTs2Ns));

  EXPECT_DOUBLE_EQ(mgr.realized_pnl(kTicker), kExpectedPnl5Pairs);
  EXPECT_DOUBLE_EQ(mgr.realized_pnl(kOtherTicker), 0.0);
}

// ---- unrealized_pnl (mark-to-market of open inventory) ----

TEST_F(OrderManagerTest, UnrealizedPnlZeroWithNoInventory) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  constexpr int kYesMid = 50;
  EXPECT_DOUBLE_EQ(mgr.unrealized_pnl(kTicker, kYesMid), 0.0);
}

TEST_F(OrderManagerTest, UnrealizedPnlOnLongYesInventory) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  // Hold 5 YES bought @ 52. Mark at YES mid 60 => (60 - 52) * 5 = 40 cents.
  constexpr int kYesMid = 60;
  constexpr double kExpectedUnrealized = (kYesMid - kYesFillPrice) * kFillQty;
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFillQty, kTs1Ns));

  EXPECT_DOUBLE_EQ(mgr.unrealized_pnl(kTicker, kYesMid), kExpectedUnrealized);
}

TEST_F(OrderManagerTest, UnrealizedPnlOnLongNoInventoryUsesComplementMid) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  // Hold 5 NO bought @ 44. NO mark = 100 - yes_mid. At yes_mid 50, NO mark
  // = 50. Unrealized = (50 - 44) * 5 = 30 cents.
  constexpr int kYesMid = 50;
  constexpr int kNoMark = kContractCents - kYesMid;
  constexpr double kExpectedUnrealized = (kNoMark - kNoFillPrice) * kFillQty;
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::No, kNoFillPrice,
                            kFillQty, kTs1Ns));

  EXPECT_DOUBLE_EQ(mgr.unrealized_pnl(kTicker, kYesMid), kExpectedUnrealized);
}

TEST_F(OrderManagerTest, UnrealizedPnlExcludesMatchedInventory) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  // 5 YES @ 52 then 5 NO @ 44 fully match -> no open inventory left.
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFillQty, kTs1Ns));
  mgr.record_fill(make_fill(kOrderId2, kTicker, kalshi::Side::No, kNoFillPrice,
                            kFillQty, kTs2Ns));

  constexpr int kYesMid = 70;
  EXPECT_DOUBLE_EQ(mgr.unrealized_pnl(kTicker, kYesMid), 0.0);
}

// ---- position_cost (capital at risk in open inventory) ----

TEST_F(OrderManagerTest, PositionCostIsZeroWithNoInventory) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  EXPECT_DOUBLE_EQ(mgr.position_cost(kTicker), 0.0);
}

TEST_F(OrderManagerTest, PositionCostSumsOpenLotCostBasis) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  // Hold 5 YES @ 52 => capital at risk = 52 * 5 = 260 cents.
  constexpr double kExpectedCost =
      static_cast<double>(kYesFillPrice) * kFillQty;
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFillQty, kTs1Ns));

  EXPECT_DOUBLE_EQ(mgr.position_cost(kTicker), kExpectedCost);
}

// ---- exposure() / E_win decomposition ----

TEST_F(OrderManagerTest, ExposureZeroWithNoPosition) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};

  const auto exposure = mgr.exposure_decomposition(kTicker);

  EXPECT_EQ(exposure.net_inventory, 0);
  EXPECT_DOUBLE_EQ(exposure.spread_capture_cents, 0.0);
  EXPECT_DOUBLE_EQ(exposure.e_win_cents, 0.0);
  EXPECT_DOUBLE_EQ(exposure.e_loss_cents, 0.0);
}

TEST_F(OrderManagerTest, ExposureOnNetLongYes) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFillQty, kTs1Ns));

  const auto exposure = mgr.exposure_decomposition(kTicker);
  const double cost = static_cast<double>(kYesFillPrice) * kFillQty; // 260

  EXPECT_EQ(exposure.net_inventory, kFillQty);
  EXPECT_DOUBLE_EQ(exposure.spread_capture_cents, 0.0);
  // If YES wins: 5 * 100 - 260 = 240; if it loses: -260 (the cost).
  EXPECT_DOUBLE_EQ(exposure.e_win_cents,
                   static_cast<double>(kFillQty) * kContractPayout - cost);
  EXPECT_DOUBLE_EQ(exposure.e_loss_cents, -cost);
}

TEST_F(OrderManagerTest, ExposureOnNetLongNo) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::No, kNoFillPrice,
                            kFillQty, kTs1Ns));

  const auto exposure = mgr.exposure_decomposition(kTicker);
  const double cost = static_cast<double>(kNoFillPrice) * kFillQty; // 220

  EXPECT_EQ(exposure.net_inventory, -kFillQty);
  EXPECT_DOUBLE_EQ(exposure.e_win_cents,
                   static_cast<double>(kFillQty) * kContractPayout - cost);
  EXPECT_DOUBLE_EQ(exposure.e_loss_cents, -cost);
}

TEST_F(OrderManagerTest, ExposureSplitsMatchedSpreadFromDirectional) {
  auto rest_client = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager mgr{rest_client};
  // 5 YES @ 52, then 3 NO @ 44 → 3 contracts offset (spread capture), 2 YES
  // open.
  mgr.record_fill(make_fill(kOrderId, kTicker, kalshi::Side::Yes, kYesFillPrice,
                            kFillQty, kTs1Ns));
  mgr.record_fill(make_fill(kOrderId2, kTicker, kalshi::Side::No, kNoFillPrice,
                            kNoMatchQty, kTs2Ns));

  const auto exposure = mgr.exposure_decomposition(kTicker);
  const double spread =
      static_cast<double>(kContractPayout - kNoFillPrice - kYesFillPrice) *
      kNoMatchQty;                              // (100-44-52)*3 = 12
  const int remaining = kFillQty - kNoMatchQty; // 2
  const double cost = static_cast<double>(kYesFillPrice) * remaining; // 104

  EXPECT_EQ(exposure.net_inventory, remaining);
  EXPECT_DOUBLE_EQ(exposure.spread_capture_cents, spread);
  EXPECT_DOUBLE_EQ(exposure.e_win_cents,
                   static_cast<double>(remaining) * kContractPayout - cost);
  EXPECT_DOUBLE_EQ(exposure.e_loss_cents, -cost);
}
