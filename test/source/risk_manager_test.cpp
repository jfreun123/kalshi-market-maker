#include "fake_transport.hpp"
#include "order_manager.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"

#include <gtest/gtest.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// ---- Test constants ----

namespace {

constexpr int kValidPrice = 52;
constexpr int kSmallQty = 5;
constexpr int kMaxOrderSize = 25;
constexpr int kOverMaxOrderSize = 26;
constexpr int kMaxPosition = 100;
constexpr int kLossPriceYes = 90;
constexpr int kLossPriceNo = 80;
// YES@90 + NO@80 → PnL = 100-90-80 = -70 cents = -$0.70 per pair.
// kTightDailyLossLimit = -$0.50, so one such pair triggers the halt.
constexpr double kTightDailyLossLimit = -0.50;
// A YES@90 qty-1 fill costs 90 cents = $0.90 in capital at risk.
// kTightExposureLimit = $0.50, so a single such fill trips over-exposure.
constexpr double kTightExposureLimit = 0.50;
// kTightTotalLoss = -$0.50; a -70c mark-to-market loss trips the portfolio
// halt.
constexpr double kTightTotalLoss = -0.50;
constexpr double kNotionalAboveExposureCents = 90.0; // $0.90 > $0.50 cap
constexpr double kUnrealizedLossCents = -70.0;       // -$0.70 < -$0.50 cap
// Drawdown: peak +$5.00, give back to +$3.00 = $2.00 drawn down > $1.00 cap.
constexpr double kTightDrawdown = 1.00;          // dollars (positive)
constexpr double kPeakPnlCents = 500.0;          // +$5.00 high-water mark
constexpr double kGivebackPnlCents = 300.0;      // +$3.00 (still net positive)
constexpr double kSmallGivebackPnlCents = 450.0; // +$4.50 (only $0.50 back)
constexpr long long kTs1Ns = 1'000'000LL;
constexpr long long kTs2Ns = 2'000'000LL;
constexpr int kHttpOk = 200;

const std::string kTicker = "KXBTCD";
const std::string kOrderId1 = "order-001";
const std::string kOrderId2 = "order-002";
const std::string kOrderId3 = "order-003";
const std::string kOrderId4 = "order-004";
const std::string kApiKey = "test-key-id";
const std::string kBaseUrl = "https://trading-api.kalshi.com/trade-api/v2";

// RSA key generated once per test suite.
std::string
    kPemPrivateKey; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

kalshi::RiskLimits default_limits() { return kalshi::RiskLimits{}; }

kalshi::RiskLimits tight_exposure_limits() {
  kalshi::RiskLimits limits;
  limits.max_total_exposure_dollars = kTightExposureLimit;
  return limits;
}

kalshi::RiskLimits tight_loss_limits() {
  kalshi::RiskLimits limits;
  limits.daily_loss_limit = kTightDailyLossLimit;
  return limits;
}

kalshi::RiskLimits tight_total_loss_limits() {
  kalshi::RiskLimits limits;
  limits.max_total_loss_dollars = kTightTotalLoss;
  return limits;
}

kalshi::RiskLimits tight_drawdown_limits() {
  kalshi::RiskLimits limits;
  limits.max_drawdown_dollars = kTightDrawdown;
  return limits;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kalshi::PortfolioSnapshot make_snapshot(double realized_cents,
                                        double unrealized_cents,
                                        double notional_cents) {
  kalshi::PortfolioSnapshot snapshot;
  snapshot.total_realized_cents = realized_cents;
  snapshot.total_unrealized_cents = unrealized_cents;
  snapshot.total_notional_cents = notional_cents;
  return snapshot;
}

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

std::string order_response_json(const std::string &order_id, int qty) {
  return R"({"order_id":")" + order_id +
         R"(","fill_count":"0.00","remaining_count":")" + std::to_string(qty) +
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
      std::chrono::nanoseconds{timestamp_ns}};
  return fill;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace

// ---- Test fixture ----

class RiskManagerTest : public ::testing::Test {
public:
  static void SetUpTestSuite() { kPemPrivateKey = generate_rsa_pem(); }
};

// ---- Helpers used within tests ----

namespace {

kalshi::RestClient make_rest_client(std::unique_ptr<FakeTransport> transport) {
  return kalshi::RestClient{kalshi::Auth{kApiKey, kPemPrivateKey},
                            std::move(transport), kBaseUrl};
}

} // namespace

// ---- Initial state ----

TEST_F(RiskManagerTest, NotHaltedInitially) {
  kalshi::RiskManager risk_mgr{default_limits()};
  EXPECT_FALSE(risk_mgr.is_halted());
}

TEST_F(RiskManagerTest, CheckOrderPassesWithinAllLimits) {
  kalshi::RiskManager risk_mgr{default_limits()};
  EXPECT_TRUE(
      risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty));
}

// ---- Order size limit ----

TEST_F(RiskManagerTest, RejectsOrderExceedingMaxSize) {
  kalshi::RiskManager risk_mgr{default_limits()};
  EXPECT_FALSE(risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice,
                                    kOverMaxOrderSize));
}

TEST_F(RiskManagerTest, AcceptsOrderAtExactMaxSize) {
  kalshi::RiskManager risk_mgr{default_limits()};
  EXPECT_TRUE(risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice,
                                   kMaxOrderSize));
}

// ---- Price-range gate ----

TEST_F(RiskManagerTest, RejectsOrderBelowMinQuotePrice) {
  const kalshi::RiskLimits limits;
  kalshi::RiskManager risk_mgr{limits};
  const int below = limits.min_quote_price_cents - 1; // cheap longshot
  EXPECT_FALSE(
      risk_mgr.check_order(kTicker, kalshi::Side::Yes, below, kSmallQty));
}

TEST_F(RiskManagerTest, RejectsOrderAboveMaxQuotePrice) {
  const kalshi::RiskLimits limits;
  kalshi::RiskManager risk_mgr{limits};
  const int above = limits.max_quote_price_cents + 1;
  EXPECT_FALSE(
      risk_mgr.check_order(kTicker, kalshi::Side::No, above, kSmallQty));
}

TEST_F(RiskManagerTest, AcceptsOrderAtQuotePriceBandEdges) {
  const kalshi::RiskLimits limits;
  kalshi::RiskManager risk_mgr{limits};
  EXPECT_TRUE(risk_mgr.check_order(kTicker, kalshi::Side::Yes,
                                   limits.min_quote_price_cents, kSmallQty));
  EXPECT_TRUE(risk_mgr.check_order(kTicker, kalshi::Side::Yes,
                                   limits.max_quote_price_cents, kSmallQty));
}

// ---- Position limit (from zero, no update() needed) ----

TEST_F(RiskManagerTest, RejectsYesOrderExceedingPositionLimitFromZero) {
  kalshi::RiskLimits limits;
  limits.max_position_per_market = kMaxPosition;
  kalshi::RiskManager risk_mgr{limits};
  // abs(0 + (kMaxPosition+1)) > kMaxPosition → rejected.
  const int over_limit = kMaxPosition + 1;
  EXPECT_FALSE(risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice,
                                    over_limit));
}

TEST_F(RiskManagerTest, RejectsNoOrderExceedingPositionLimitFromZero) {
  kalshi::RiskLimits limits;
  limits.max_position_per_market = kMaxPosition;
  kalshi::RiskManager risk_mgr{limits};
  const int over_limit = kMaxPosition + 1;
  EXPECT_FALSE(
      risk_mgr.check_order(kTicker, kalshi::Side::No, kValidPrice, over_limit));
}

// ---- Accumulated position via update() ----

TEST_F(RiskManagerTest, AccumulatedPositionBlocksFurtherSameSideOrders) {
  // Fill kMaxPosition YES contracts so net_position == kMaxPosition after
  // update.
  auto rest = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager order_mgr{rest};
  order_mgr.record_fill(make_fill(kOrderId1, kTicker, kalshi::Side::Yes,
                                  kValidPrice, kMaxPosition, kTs1Ns));

  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.update(order_mgr, {kTicker});

  // Position is 100 (= limit). Any YES order now would push abs(pos) over.
  EXPECT_FALSE(
      risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice, 1));
}

TEST_F(RiskManagerTest, AccumulatedPositionAllowsReducingOrders) {
  auto rest = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager order_mgr{rest};
  order_mgr.record_fill(make_fill(kOrderId1, kTicker, kalshi::Side::Yes,
                                  kValidPrice, kMaxPosition, kTs1Ns));

  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.update(order_mgr, {kTicker});

  // A NO order reduces position: abs(100 - 1) = 99 <= 100 → allowed.
  EXPECT_TRUE(risk_mgr.check_order(kTicker, kalshi::Side::No, kValidPrice, 1));
}

// ---- Open order count via update() ----

TEST_F(RiskManagerTest, RejectsOrderWhenAtOpenOrderLimit) {
  // Fill the open order limit (4); one more order must be rejected.
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId1, kSmallQty)});
  transport->enqueue({kHttpOk, order_response_json(kOrderId2, kSmallQty)});
  transport->enqueue({kHttpOk, order_response_json(kOrderId3, kSmallQty)});
  transport->enqueue({kHttpOk, order_response_json(kOrderId4, kSmallQty)});
  auto rest = make_rest_client(std::move(transport));
  kalshi::OrderManager order_mgr{rest};
  order_mgr.place(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty);
  order_mgr.place(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty);
  order_mgr.place(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty);
  order_mgr.place(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty);

  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.update(order_mgr, {kTicker});

  EXPECT_FALSE(
      risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty));
}

TEST_F(RiskManagerTest, AcceptsOrderWhenBelowOpenOrderLimit) {
  // 3 open orders < limit of 4; another order is allowed.
  auto transport = std::make_unique<FakeTransport>();
  transport->enqueue({kHttpOk, order_response_json(kOrderId1, kSmallQty)});
  transport->enqueue({kHttpOk, order_response_json(kOrderId2, kSmallQty)});
  transport->enqueue({kHttpOk, order_response_json(kOrderId3, kSmallQty)});
  auto rest = make_rest_client(std::move(transport));
  kalshi::OrderManager order_mgr{rest};
  order_mgr.place(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty);
  order_mgr.place(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty);
  order_mgr.place(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty);

  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.update(order_mgr, {kTicker});

  EXPECT_TRUE(
      risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty));
}

// ---- Manual halt / resume ----

TEST_F(RiskManagerTest, IsHaltedAfterManualHalt) {
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.halt();
  EXPECT_TRUE(risk_mgr.is_halted());
}

TEST_F(RiskManagerTest, CheckOrderReturnsFalseWhenHalted) {
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.halt();
  EXPECT_FALSE(
      risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty));
}

TEST_F(RiskManagerTest, ResumeRestoresAfterManualHalt) {
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.halt();
  risk_mgr.resume();
  EXPECT_FALSE(risk_mgr.is_halted());
  EXPECT_TRUE(
      risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty));
}

// ---- Daily loss limit via update() ----

TEST_F(RiskManagerTest, UpdateHaltsWhenDailyLossExceeded) {
  // YES@90 fills, then NO@80 matches: PnL = 100-90-80 = -70 cents = -$0.70.
  // kTightDailyLossLimit = -$0.50, so -$0.70 < -$0.50 → auto-halt.
  auto rest = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager order_mgr{rest};
  order_mgr.record_fill(make_fill(kOrderId1, kTicker, kalshi::Side::Yes,
                                  kLossPriceYes, 1, kTs1Ns));
  order_mgr.record_fill(
      make_fill(kOrderId2, kTicker, kalshi::Side::No, kLossPriceNo, 1, kTs2Ns));

  kalshi::RiskManager risk_mgr{tight_loss_limits()};
  risk_mgr.update(order_mgr, {kTicker});

  EXPECT_TRUE(risk_mgr.is_halted());
}

TEST_F(RiskManagerTest, UpdateDoesNotHaltWithinLimits) {
  // Empty order manager → PnL = 0 → above the tight loss limit.
  auto rest = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager order_mgr{rest};

  kalshi::RiskManager risk_mgr{tight_loss_limits()};
  risk_mgr.update(order_mgr, {kTicker});

  EXPECT_FALSE(risk_mgr.is_halted());
}

// ---- Over-exposure via update_portfolio() ----

TEST_F(RiskManagerTest, UpdatePortfolioHaltsWhenTotalExposureExceeded) {
  // $0.90 capital at risk, above the $0.50 cap → over-exposure halt.
  kalshi::RiskManager risk_mgr{tight_exposure_limits()};
  risk_mgr.update_portfolio(
      make_snapshot(0.0, 0.0, kNotionalAboveExposureCents));

  EXPECT_TRUE(risk_mgr.is_halted());
  EXPECT_TRUE(risk_mgr.is_set(kalshi::Constraint::kOverExposure));
}

TEST_F(RiskManagerTest, UpdatePortfolioDoesNotHaltWhenExposureWithinLimit) {
  kalshi::RiskManager risk_mgr{tight_exposure_limits()};
  risk_mgr.update_portfolio(make_snapshot(0.0, 0.0, 0.0));

  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kOverExposure));
}

// ---- Portfolio total-loss kill-switch via update_portfolio() ----

TEST_F(RiskManagerTest, UpdatePortfolioHaltsOnUnrealizedDrawdown) {
  // No realized loss, but -$0.70 mark-to-market on open inventory trips the
  // -$0.50 portfolio loss cap. The realized-only daily limit would miss this.
  kalshi::RiskManager risk_mgr{tight_total_loss_limits()};
  risk_mgr.update_portfolio(make_snapshot(0.0, kUnrealizedLossCents, 0.0));

  EXPECT_TRUE(risk_mgr.is_halted());
  EXPECT_TRUE(risk_mgr.is_set(kalshi::Constraint::kPortfolioLoss));
}

TEST_F(RiskManagerTest, UpdatePortfolioDoesNotHaltWhenTotalPnlWithinLimit) {
  kalshi::RiskManager risk_mgr{tight_total_loss_limits()};
  risk_mgr.update_portfolio(make_snapshot(0.0, 0.0, 0.0));

  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kPortfolioLoss));
}

TEST_F(RiskManagerTest, UpdatePortfolioDefaultLimitsDoNotHalt) {
  // Modest exposure and a small unrealized loss stay within the generous
  // defaults → no portfolio constraint set.
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.update_portfolio(
      make_snapshot(0.0, kUnrealizedLossCents, kNotionalAboveExposureCents));

  EXPECT_FALSE(risk_mgr.is_halted());
}

// ---- Drawdown kill-switch (high-water mark) ----

TEST_F(RiskManagerTest, UpdatePortfolioHaltsOnDrawdownFromPeak) {
  kalshi::RiskManager risk_mgr{tight_drawdown_limits()};
  // Climb to a +$5.00 high-water mark, then give back $2.00 (> $1.00 cap).
  risk_mgr.update_portfolio(make_snapshot(kPeakPnlCents, 0.0, 0.0));
  ASSERT_FALSE(risk_mgr.is_halted());
  risk_mgr.update_portfolio(make_snapshot(kGivebackPnlCents, 0.0, 0.0));

  EXPECT_TRUE(risk_mgr.is_set(kalshi::Constraint::kDrawdown));
  // Fires even though total PnL is still net positive — a drawdown, not a loss.
  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kPortfolioLoss));
}

TEST_F(RiskManagerTest, UpdatePortfolioNoDrawdownHaltWithinLimit) {
  kalshi::RiskManager risk_mgr{tight_drawdown_limits()};
  risk_mgr.update_portfolio(make_snapshot(kPeakPnlCents, 0.0, 0.0));
  risk_mgr.update_portfolio(make_snapshot(kSmallGivebackPnlCents, 0.0, 0.0));

  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kDrawdown));
}

TEST_F(RiskManagerTest, ResumeReanchorsDrawdownPeak) {
  kalshi::RiskManager risk_mgr{tight_drawdown_limits()};
  risk_mgr.update_portfolio(make_snapshot(kPeakPnlCents, 0.0, 0.0));
  risk_mgr.update_portfolio(make_snapshot(kGivebackPnlCents, 0.0, 0.0));
  ASSERT_TRUE(risk_mgr.is_set(kalshi::Constraint::kDrawdown));

  risk_mgr.resume();
  // The same depressed PnL must not immediately re-trip — peak re-anchored.
  risk_mgr.update_portfolio(make_snapshot(kGivebackPnlCents, 0.0, 0.0));

  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kDrawdown));
  EXPECT_FALSE(risk_mgr.is_halted());
}

TEST_F(RiskManagerTest, ResumeRestoresAfterAutoHalt) {
  auto rest = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager order_mgr{rest};
  order_mgr.record_fill(make_fill(kOrderId1, kTicker, kalshi::Side::Yes,
                                  kLossPriceYes, 1, kTs1Ns));
  order_mgr.record_fill(
      make_fill(kOrderId2, kTicker, kalshi::Side::No, kLossPriceNo, 1, kTs2Ns));

  kalshi::RiskManager risk_mgr{tight_loss_limits()};
  risk_mgr.update(order_mgr, {kTicker});
  ASSERT_TRUE(risk_mgr.is_halted());

  risk_mgr.resume();
  EXPECT_FALSE(risk_mgr.is_halted());
  EXPECT_TRUE(
      risk_mgr.check_order(kTicker, kalshi::Side::Yes, kValidPrice, kSmallQty));
}

// ---- Constraint bitset ----

TEST_F(RiskManagerTest, HaltSetsManualHaltConstraint) {
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.halt();
  EXPECT_TRUE(risk_mgr.is_set(kalshi::Constraint::kManualHalt));
}

TEST_F(RiskManagerTest, SetAndClearIndividualConstraint) {
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.set(kalshi::Constraint::kHighFillRate);
  EXPECT_TRUE(risk_mgr.is_set(kalshi::Constraint::kHighFillRate));
  EXPECT_TRUE(risk_mgr.is_halted());
  risk_mgr.clear(kalshi::Constraint::kHighFillRate);
  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kHighFillRate));
  EXPECT_FALSE(risk_mgr.is_halted());
}

TEST_F(RiskManagerTest, MultipleConstraintBitsAreIndependent) {
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.set(kalshi::Constraint::kManualHalt);
  risk_mgr.set(kalshi::Constraint::kHighFillRate);
  EXPECT_TRUE(risk_mgr.is_halted());

  risk_mgr.clear(kalshi::Constraint::kManualHalt);
  EXPECT_TRUE(risk_mgr.is_halted()); // kHighFillRate still set
  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kManualHalt));

  risk_mgr.clear(kalshi::Constraint::kHighFillRate);
  EXPECT_FALSE(risk_mgr.is_halted());
}

TEST_F(RiskManagerTest, ResumesClearsAllConstraints) {
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.set(kalshi::Constraint::kManualHalt);
  risk_mgr.set(kalshi::Constraint::kHighFillRate);
  risk_mgr.resume();
  EXPECT_FALSE(risk_mgr.is_halted());
  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kManualHalt));
  EXPECT_FALSE(risk_mgr.is_set(kalshi::Constraint::kHighFillRate));
}

TEST_F(RiskManagerTest, PnlLimitBreachSetsKPnLLimitConstraint) {
  auto rest = make_rest_client(std::make_unique<FakeTransport>());
  kalshi::OrderManager order_mgr{rest};
  order_mgr.record_fill(make_fill(kOrderId1, kTicker, kalshi::Side::Yes,
                                  kLossPriceYes, 1, kTs1Ns));
  order_mgr.record_fill(
      make_fill(kOrderId2, kTicker, kalshi::Side::No, kLossPriceNo, 1, kTs2Ns));

  kalshi::RiskManager risk_mgr{tight_loss_limits()};
  risk_mgr.update(order_mgr, {kTicker});

  EXPECT_TRUE(risk_mgr.is_set(kalshi::Constraint::kPnLLimit));
}

TEST_F(RiskManagerTest, ActiveConstraintsReturnsSetBitNames) {
  kalshi::RiskManager risk_mgr{default_limits()};
  risk_mgr.set(kalshi::Constraint::kManualHalt);
  const std::string active = risk_mgr.active_constraints();
  EXPECT_NE(active.find("kManualHalt"), std::string::npos);
}

TEST_F(RiskManagerTest, ActiveConstraintsEmptyWhenNoneSet) {
  kalshi::RiskManager risk_mgr{default_limits()};
  EXPECT_TRUE(risk_mgr.active_constraints().empty());
}
