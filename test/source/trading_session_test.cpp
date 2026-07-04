#include "fake_transport.hpp"
#include "order_manager.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"
#include "trading_session.hpp"
#include "types.hpp"

#include <gtest/gtest.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

// ---- Test constants ----

namespace {

// Orderbook: YES bid=51, NO bid=47 → YES ask=53, mid=(51+53)/2=52.
constexpr int kYesBid = 51;
constexpr int kNoBid = 47;
constexpr int kObQty = 100;

// Delta below best bid — keeps a valid BBO so the quoter places quotes.
constexpr int kSubBboDeltaPrice = 50;
constexpr kalshi::Quantity kSubBboDeltaQty =
    kalshi::Quantity::from_contracts(100);
constexpr int kDefaultQuoteSize = kalshi::QuoterConfig::kDefaultQuoteSize;

// A YES@90 qty-1 fill leaves 90c = $0.90 capital at risk.
constexpr int kHighYesPrice = 90;
constexpr int kOneLot = 1;
constexpr double kTightExposureDollars = 0.50; // $0.50 < $0.90 → over-exposed

const std::string kTicker = "KXBTCD";
const std::string kOrderId1 = "order-001";
const std::string kOrderId2 = "order-002";
const std::string kOrderId3 = "order-003";
const std::string kOrderId4 = "order-004";
const std::string kFillOrderId = "fill-001";
const std::string kApiKey = "test-key-id";
const std::string kBaseUrl = "https://trading-api.kalshi.com/trade-api/v2";
constexpr int kHttpOk = 200;
constexpr int kHttpBadRequest = 400;
constexpr std::string_view kPostOnlyCrossBody =
    R"({"error":{"code":"invalid_order","details":"post only cross"}})";

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::string kPemPrivateKey;

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

// Minimal V2 order response so OrderManager tracks the placed order.
std::string order_json(const std::string &order_id, int qty) {
  return R"({"order_id":")" + order_id +
         R"(","fill_count":"0.00","remaining_count":")" + std::to_string(qty) +
         R"(.00","ts_ms":1718000000000})";
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
kalshi::Orderbook make_orderbook(const std::string &ticker, int yes_bid,
                                 int no_bid, int qty) {
  kalshi::Orderbook book;
  book.ticker = ticker;
  book.yes = {kalshi::Level{yes_bid, kalshi::Quantity::from_contracts(qty)}};
  book.no = {kalshi::Level{no_bid, kalshi::Quantity::from_contracts(qty)}};
  return book;
}

kalshi::Fill make_fill(const std::string &order_id, const std::string &ticker,
                       kalshi::Side side, int price_cents, int quantity) {
  kalshi::Fill fill;
  fill.order_id = order_id;
  fill.market_ticker = ticker;
  fill.side = side;
  fill.price_cents = price_cents;
  fill.quantity = kalshi::Quantity::from_contracts(quantity);
  fill.timestamp = std::chrono::system_clock::time_point{};
  return fill;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

int count_method(const FakeTransport &transport, const std::string &method) {
  const auto &requests = transport.recorded_requests();
  return static_cast<int>(
      std::count_if(requests.begin(), requests.end(),
                    [&method](const FakeTransport::RecordedRequest &request) {
                      return request.method == method;
                    }));
}

} // namespace

// ---- Fixture: bundles the full domain stack over a FakeTransport ----

class TradingSessionTest : public ::testing::Test {
public:
  static void SetUpTestSuite() { kPemPrivateKey = generate_rsa_pem(); }

  explicit TradingSessionTest(kalshi::RiskLimits limits = kalshi::RiskLimits{})
      : transport_owner_{std::make_unique<FakeTransport>()},
        transport_{*transport_owner_},
        rest_{kalshi::Auth{kApiKey, kPemPrivateKey},
              std::move(transport_owner_), kBaseUrl},
        order_mgr_{rest_}, risk_mgr_{limits},
        quoter_{kalshi::QuoterConfig{}, order_mgr_, risk_mgr_},
        session_{std::vector<std::string>{kTicker}, order_mgr_, risk_mgr_,
                 quoter_} {}

  // Public so TEST_F bodies (generated subclasses) reach them; all members
  // public keeps the fixture exempt from the member-visibility check.
  std::unique_ptr<FakeTransport> transport_owner_;
  FakeTransport &transport_;
  kalshi::RestClient rest_;
  kalshi::OrderManager order_mgr_;
  kalshi::RiskManager risk_mgr_;
  kalshi::Quoter quoter_;
  kalshi::TradingSession session_;
};

class TradingSessionTightExposureTest : public TradingSessionTest {
protected:
  TradingSessionTightExposureTest() : TradingSessionTest{tight_limits()} {}

private:
  static kalshi::RiskLimits tight_limits() {
    kalshi::RiskLimits limits;
    limits.max_total_exposure_dollars = kTightExposureDollars;
    return limits;
  }
};

// ---- Tests ----

TEST_F(TradingSessionTest, SnapshotThenDeltaPlacesQuotes) {
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});

  session_.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);

  // One bid POST + one ask POST.
  EXPECT_EQ(count_method(transport_, "POST"), 2);
}

TEST_F(TradingSessionTest, PlaceErrorCoolsDownTickerThenResumes) {
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  constexpr auto kCooldown = std::chrono::milliseconds{500};
  kalshi::TradingSession session{std::vector<std::string>{kTicker},
                                 order_mgr_,
                                 risk_mgr_,
                                 quoter_,
                                 nullptr,
                                 [clock_now] { return *clock_now; },
                                 kCooldown};

  session.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));

  transport_.enqueue({kHttpBadRequest, std::string{kPostOnlyCrossBody}});
  session.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                   kSubBboDeltaQty);
  const int posts_after_reject = count_method(transport_, "POST");
  ASSERT_GE(posts_after_reject, 1);

  session.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                   kSubBboDeltaQty);
  EXPECT_EQ(count_method(transport_, "POST"), posts_after_reject)
      << "ticker in cooldown — should not re-quote the same crossing price";

  *clock_now += kCooldown + std::chrono::milliseconds{1};
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});
  session.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                   kSubBboDeltaQty);
  EXPECT_GT(count_method(transport_, "POST"), posts_after_reject)
      << "cooldown elapsed — quoting resumes";
}

TEST_F(TradingSessionTest, BookAgeTracksTimeSinceLastBookUpdate) {
  // A paused market sends no deltas; book_age is the observable that
  // distinguishes idle-because-quiet from wedged (demo finding D8).
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::TradingSession session{std::vector<std::string>{kTicker},
                                 order_mgr_,
                                 risk_mgr_,
                                 quoter_,
                                 nullptr,
                                 [clock_now] { return *clock_now; }};

  EXPECT_FALSE(session.book_age(kTicker).has_value())
      << "no book yet -> no age";

  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});
  session.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));

  constexpr auto kQuietSpell = std::chrono::seconds{90};
  *clock_now += kQuietSpell;
  ASSERT_TRUE(session.book_age(kTicker).has_value());
  EXPECT_EQ(session.book_age(kTicker).value(), kQuietSpell);

  session.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                   kSubBboDeltaQty);
  EXPECT_EQ(session.book_age(kTicker).value(), std::chrono::seconds{0});
}

TEST_F(TradingSessionTest, OrderbooksAccessorReflectsSnapshot) {
  session_.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));

  const auto &books = session_.orderbooks();
  ASSERT_TRUE(books.contains(kTicker));
  EXPECT_TRUE(books.at(kTicker).best_bid().has_value());
  EXPECT_TRUE(books.at(kTicker).best_ask().has_value());
}

TEST_F(TradingSessionTest, CancelPreexistingOrdersCancelsTrackedTickerOrphans) {
  kalshi::Order tracked_orphan;
  tracked_orphan.id = kOrderId1;
  tracked_orphan.market_ticker = kTicker;
  kalshi::Order untracked_orphan;
  untracked_orphan.id = kOrderId2;
  untracked_orphan.market_ticker = "KXOTHER-MARKET";

  session_.cancel_preexisting_orders({tracked_orphan, untracked_orphan});

  EXPECT_EQ(count_method(transport_, "DELETE"), 1);
  bool cancelled_tracked = false;
  for (const auto &request : transport_.recorded_requests()) {
    if (request.method == "DELETE" &&
        request.url.find(kOrderId1) != std::string::npos) {
      cancelled_tracked = true;
    }
  }
  EXPECT_TRUE(cancelled_tracked);
}

TEST_F(TradingSessionTest, CancelPreexistingOrdersNoOpWhenNoneResting) {
  session_.cancel_preexisting_orders({});
  EXPECT_EQ(count_method(transport_, "DELETE"), 0);
}

TEST_F(TradingSessionTest, FullyFilledQuoteSideRequotesOnNextDelta) {
  // A complete fill removes the resting order from the exchange. The quoter
  // must forget its id and re-place that side — not sit dark believing a quote
  // is still live (the phantom-quote livelock).
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});
  session_.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);
  ASSERT_EQ(count_method(transport_, "POST"), 2); // bid + ask resting

  // Fully fill the bid (kOrderId1 was the first placement).
  session_.on_fill(make_fill(kOrderId1, kTicker, kalshi::Side::Yes, kYesBid,
                             kDefaultQuoteSize));

  transport_.enqueue({kHttpOk, order_json(kOrderId3, kDefaultQuoteSize)});
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);

  EXPECT_EQ(count_method(transport_, "POST"), 3)
      << "bid side must re-quote after its order fully filled";
}

TEST_F(TradingSessionTest, PartiallyFilledQuoteSideIsNotReplaced) {
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});
  session_.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);
  ASSERT_EQ(count_method(transport_, "POST"), 2);

  // Partial fill: the order still rests on the exchange, so the quoter must
  // keep tracking it rather than re-placing a duplicate.
  session_.on_fill(
      make_fill(kOrderId1, kTicker, kalshi::Side::Yes, kYesBid, kOneLot));

  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);

  EXPECT_EQ(count_method(transport_, "POST"), 2)
      << "a partially filled quote still rests — no duplicate placement";
}

TEST_F(TradingSessionTest, FillUpdatesPositionAndNotifiesPnlListener) {
  kalshi::TradingSession::PnlMap persisted;
  bool notified = false;
  session_.set_pnl_listener(
      [&notified, &persisted](const kalshi::TradingSession::PnlMap &totals) {
        notified = true;
        persisted = totals;
      });

  session_.on_fill(make_fill(kFillOrderId, kTicker, kalshi::Side::Yes,
                             kHighYesPrice, kOneLot));

  EXPECT_EQ(order_mgr_.net_position(kTicker),
            kalshi::Quantity::from_contracts(kOneLot));
  EXPECT_TRUE(notified);
  EXPECT_TRUE(session_.prior_pnl().empty()); // prior stays prior-only
  EXPECT_TRUE(persisted.empty()); // an opening fill realizes nothing yet
}

TEST_F(TradingSessionTest, CarriedPnlDoesNotDoubleCountAcrossFills) {
  // Two YES@52 + NO@44 round trips realize +4c/contract each. With one lot per
  // fill the cumulative session PnL after round trip N is 4c x N; the persisted
  // total must be prior + cumulative, NOT a running sum of running sums.
  constexpr double kPriorCents = 100.0;
  constexpr double kPairPnlCents = 4.0;
  constexpr int kYesPrice = 52;
  constexpr int kNoPrice = 44;
  session_.set_prior_pnl({{kTicker, kPriorCents}});

  kalshi::TradingSession::PnlMap persisted;
  session_.set_pnl_listener(
      [&persisted](const kalshi::TradingSession::PnlMap &totals) {
        persisted = totals;
      });

  session_.on_fill(
      make_fill("rt1-yes", kTicker, kalshi::Side::Yes, kYesPrice, kOneLot));
  session_.on_fill(
      make_fill("rt1-no", kTicker, kalshi::Side::No, kNoPrice, kOneLot));
  ASSERT_TRUE(persisted.contains(kTicker));
  EXPECT_DOUBLE_EQ(persisted.at(kTicker), kPriorCents + kPairPnlCents);

  session_.on_fill(
      make_fill("rt2-yes", kTicker, kalshi::Side::Yes, kYesPrice, kOneLot));
  session_.on_fill(
      make_fill("rt2-no", kTicker, kalshi::Side::No, kNoPrice, kOneLot));
  EXPECT_DOUBLE_EQ(persisted.at(kTicker), kPriorCents + (2.0 * kPairPnlCents));

  EXPECT_DOUBLE_EQ(session_.prior_pnl().at(kTicker), kPriorCents);
}

TEST_F(TradingSessionTest, RecordFlattenRealizesPnlAndNotifiesListener) {
  // Session accumulates 10 NO @44 (no round trip → realized 0), then shutdown
  // flattens by buying 10 YES at an average fill of 60. Each complete set
  // realizes 100-60-44 = -4c, so the persisted total must move from the prior
  // alone to prior - 40c. Pre-fix the flatten execution was never recorded and
  // the session's true result vanished (demo finding D7).
  constexpr double kPriorCents = 100.0;
  constexpr int kOpenNoPrice = 44;
  constexpr int kFlattenLots = 10;
  constexpr int kAvgYesFill = 60;
  constexpr double kFlattenPnlCents = -40.0;
  session_.set_prior_pnl({{kTicker, kPriorCents}});

  kalshi::TradingSession::PnlMap persisted;
  session_.set_pnl_listener(
      [&persisted](const kalshi::TradingSession::PnlMap &totals) {
        persisted = totals;
      });

  session_.on_fill(make_fill("open-no", kTicker, kalshi::Side::No,
                             kOpenNoPrice, kFlattenLots));
  ASSERT_EQ(order_mgr_.net_position(kTicker),
            kalshi::Quantity::from_contracts(-kFlattenLots));

  kalshi::Order flatten_order;
  flatten_order.id = "flatten-1";
  flatten_order.market_ticker = kTicker;
  flatten_order.side = kalshi::Side::Yes;
  flatten_order.price_cents = kalshi::kMaxPriceCents;
  flatten_order.quantity = kalshi::Quantity::from_contracts(kFlattenLots);
  flatten_order.filled_quantity =
      kalshi::Quantity::from_contracts(kFlattenLots);
  flatten_order.status = kalshi::OrderStatus::Filled;
  flatten_order.average_fill_price_cents = kAvgYesFill;
  session_.record_flatten(flatten_order);

  EXPECT_TRUE(order_mgr_.net_position(kTicker).is_zero());
  ASSERT_TRUE(persisted.contains(kTicker));
  EXPECT_DOUBLE_EQ(persisted.at(kTicker), kPriorCents + kFlattenPnlCents);
}

TEST_F(TradingSessionTest, RecordFlattenWithNoFillIsIgnored) {
  bool notified = false;
  session_.set_pnl_listener(
      [&notified](const kalshi::TradingSession::PnlMap &) { notified = true; });

  kalshi::Order empty_flatten;
  empty_flatten.id = "flatten-empty";
  empty_flatten.market_ticker = kTicker;
  empty_flatten.side = kalshi::Side::Yes;
  empty_flatten.status = kalshi::OrderStatus::Cancelled;
  session_.record_flatten(empty_flatten);

  EXPECT_TRUE(order_mgr_.net_position(kTicker).is_zero());
  EXPECT_FALSE(notified);
}

TEST_F(TradingSessionTightExposureTest, RunPortfolioRiskHaltsOnOverExposure) {
  session_.on_fill(make_fill(kFillOrderId, kTicker, kalshi::Side::Yes,
                             kHighYesPrice, kOneLot));
  ASSERT_FALSE(risk_mgr_.is_halted()); // fill alone does not over-expose

  session_.run_portfolio_risk();

  EXPECT_TRUE(risk_mgr_.is_halted());
  EXPECT_TRUE(risk_mgr_.is_set(kalshi::Constraint::kOverExposure));
}

TEST_F(TradingSessionTest, DisconnectCancelsRestingOrders) {
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});
  session_.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);
  ASSERT_EQ(order_mgr_.open_orders().size(), 2U);

  session_.on_disconnect();

  EXPECT_EQ(count_method(transport_, "DELETE"), 2);
  EXPECT_TRUE(order_mgr_.open_orders().empty());
}

TEST_F(TradingSessionTest, SeedOrderbookContainsOrderRejection) {
  // The exchange rejects the seed quote (e.g. "post only cross" on a tight
  // book). Seeding must not throw — one un-quotable market cannot abort
  // startup — and the book must still be recorded for the live feed.
  transport_.enqueue(
      {kHttpBadRequest,
       R"({"error":{"code":"invalid_order","message":"post only cross"}})"});

  EXPECT_NO_THROW(session_.seed_orderbook(
      make_orderbook(kTicker, kYesBid, kNoBid, kObQty)));
  EXPECT_TRUE(session_.orderbooks().contains(kTicker));
}

TEST_F(TradingSessionTest, ReQuotesAfterFlattenOnFeedRecovery) {
  // Regression: a flatten (disconnect/halt) cancels the resting orders, but if
  // the quoter is not resynced it keeps the dead order ids, believes it is
  // still quoting, and never re-quotes when the feed recovers.
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});
  session_.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);
  ASSERT_EQ(order_mgr_.open_orders().size(), 2U);

  session_.on_disconnect(); // flatten + resync the quoter
  ASSERT_TRUE(order_mgr_.open_orders().empty());
  const int deletes_after_flatten = count_method(transport_, "DELETE");

  // Feed recovers; a fresh book update must re-establish quotes (and must not
  // attempt to cancel the already-dead order ids).
  transport_.enqueue({kHttpOk, order_json(kOrderId3, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId4, kDefaultQuoteSize)});
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);

  EXPECT_EQ(order_mgr_.open_orders().size(), 2U);
  EXPECT_EQ(count_method(transport_, "DELETE"), deletes_after_flatten);
}

TEST_F(TradingSessionTest, EnforceQuoteSafetyFlattensWhenHalted) {
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});
  session_.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);
  ASSERT_EQ(order_mgr_.open_orders().size(), 2U);

  risk_mgr_.halt();
  session_.enforce_quote_safety();

  EXPECT_TRUE(order_mgr_.open_orders().empty());
  EXPECT_EQ(count_method(transport_, "DELETE"), 2);
}

TEST_F(TradingSessionTest, EnforceQuoteSafetyIsNoOpWhenNotHalted) {
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});
  session_.on_snapshot(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));
  session_.on_delta(kTicker, kalshi::Side::Yes, kSubBboDeltaPrice,
                    kSubBboDeltaQty);
  ASSERT_EQ(order_mgr_.open_orders().size(), 2U);

  session_.enforce_quote_safety(); // not halted

  EXPECT_EQ(order_mgr_.open_orders().size(), 2U);
  EXPECT_EQ(count_method(transport_, "DELETE"), 0);
}

TEST_F(TradingSessionTest, SeedOrderbookPlacesInitialQuotes) {
  transport_.enqueue({kHttpOk, order_json(kOrderId1, kDefaultQuoteSize)});
  transport_.enqueue({kHttpOk, order_json(kOrderId2, kDefaultQuoteSize)});

  session_.seed_orderbook(make_orderbook(kTicker, kYesBid, kNoBid, kObQty));

  EXPECT_EQ(count_method(transport_, "POST"), 2);
}
