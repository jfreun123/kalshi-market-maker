#include "quoter_test_util.hpp"

#include "engine/drift_estimator.hpp"
#include "engine/fair_value.hpp"
#include "engine/pricing_model.hpp"
#include "engine/trade_tape.hpp"

// ---- Tests ----

TEST_F(QuoterTest, PlacesBidAndAskOnFirstUpdate) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // Two POST requests: one YES buy (bid) and one NO buy (ask).
  EXPECT_EQ(transport.recorded_requests().size(), 2U);
}

TEST_F(QuoterTest, BidPriceCalculatedFromMidAndSpread) {
  // mid=52, spread=4, half_spread=2, pos=0 → bid = round(52-2) = 50.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // kExpectedBidMid52 = 50 → "0.5000" in YES dimension.
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(std::string(kBidPriceMid52)), std::string::npos);
}

TEST_F(QuoterTest, AskNoPriceCalculatedFromMidAndSpread) {
  // mid=52, spread=4, ask=54, NO price = 100-54 = 46 → YES price = 54 =
  // "0.5400".
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // kExpectedNoAskMid52 = 46 → YES complement = 54 = "0.5400".
  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find("\"side\":\"ask\""), std::string::npos);
  EXPECT_NE(ask_body.find(std::string(kAskPriceMid52)), std::string::npos);
}

TEST_F(QuoterTest, NoOrdersPlacedWhenObHasNoBbo) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  kalshi::LocalOrderbook empty_ob;
  quoter.update(kTicker, empty_ob);

  EXPECT_TRUE(transport.recorded_requests().empty());
}

TEST_F(QuoterTest, NoOrdersPlacedWhenRiskHalted) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  risk_mgr.halt();
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  EXPECT_TRUE(transport.recorded_requests().empty());
}

TEST_F(QuoterTest, CrossedVisibleBookKeepsRestingQuotes) {
  // Run-3 anomaly (finding, item 43): during fast sweeps the book flickers
  // crossed (best ask below best bid); pricing off it drags the quote to
  // garbage (yes@28 in a 46-52 market). A crossed visible book means the
  // data is mid-transition — keep the resting quotes and wait.
  constexpr int kCrossedYesBid = 52;
  constexpr int kCrossedNoBid = 60;

  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  ASSERT_EQ(transport.recorded_requests().size(), 2U);

  quoter.update(kTicker, make_ob(kCrossedYesBid, kCrossedNoBid));

  EXPECT_EQ(transport.recorded_requests().size(), 2U)
      << "crossed book (bid 52 >= ask 40) must not trigger cancel/replace";
}

TEST_F(QuoterTest, BidRoundsDownAtFractionalFairValue) {
  // yes 51@4 / no 45@6 -> best ask 55 (size 6): micro = (51*6 + 55*4)/10 =
  // 52.6. Raw bid = 50.6: rounding to nearest would quote 51, giving away
  // 0.4c of the configured half-spread. The maker's bid must round DOWN.
  constexpr int kBidLevelPrice = 51;
  constexpr int kNoLevelPrice = 45;
  const kalshi::Quantity kBidSize = kalshi::Quantity::from_contracts(4);
  const kalshi::Quantity kAskSize = kalshi::Quantity::from_contracts(6);

  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  kalshi::Orderbook snap;
  snap.ticker = kTicker;
  snap.yes = {{kBidLevelPrice, kBidSize}};
  snap.no = {{kNoLevelPrice, kAskSize}};
  kalshi::LocalOrderbook book;
  book.apply_snapshot(snap);
  quoter.update(kTicker, book);

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(R"("price":"0.5000")"), std::string::npos)
      << "bid at fv 52.6 must floor to 50, not round to 51: " << bid_body;
}

TEST_F(QuoterTest, AskRoundsUpAtFractionalFairValue) {
  // yes 51@7 / no 46@8 -> best ask 54 (size 8): micro = (51*8 + 54*7)/15 =
  // 52.4. Raw ask = 54.4: rounding to nearest would quote 54, giving away
  // 0.4c. The maker's ask must round UP (NO order at 45 -> YES 55).
  constexpr int kBidLevelPrice = 51;
  constexpr int kNoLevelPrice = 46;
  const kalshi::Quantity kBidSize = kalshi::Quantity::from_contracts(7);
  const kalshi::Quantity kAskSize = kalshi::Quantity::from_contracts(8);

  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  kalshi::Orderbook snap;
  snap.ticker = kTicker;
  snap.yes = {{kBidLevelPrice, kBidSize}};
  snap.no = {{kNoLevelPrice, kAskSize}};
  kalshi::LocalOrderbook book;
  book.apply_snapshot(snap);
  quoter.update(kTicker, book);

  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find(R"("price":"0.5500")"), std::string::npos)
      << "ask at fv 52.4 must ceil to 55, not round to 54: " << ask_body;
}

TEST_F(QuoterTest, LongPositionShiftsBidDown) {
  // pos=+20: inv_skew=1.0 → bid = round(52-2-1) = 49, ask = round(52+2-1) = 53.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kInventoryPosition);

  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // kExpectedBidMid52Long20 = 49 → "0.4900" in YES dimension.
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(std::string(kBidPriceMid52Long20)),
            std::string::npos);
}

TEST_F(QuoterTest, LongInventoryUnwindAskQuotesAtReservation) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kInventoryPosition);

  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find("\"side\":\"ask\""), std::string::npos);
  EXPECT_NE(ask_body.find(std::string(kAskPriceMid52Long20)), std::string::npos)
      << "the inventory-reducing side must quote at the reservation price — "
         "closing risk pays up to fair value instead of demanding fresh edge";
}

TEST_F(QuoterTest, ShortPositionShiftsBidUp) {
  // pos=-15: res ≈ 53.5; the unwind bid quotes at floor(res) = 53.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::No,
                       kInventoryPosition);

  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // kExpectedBidMid52Short20 = 51 → "0.5100" in YES dimension.
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(std::string(kBidPriceMid52Short20)),
            std::string::npos);
}

TEST_F(QuoterTest, AskAlwaysHigherThanBidWithExtremeInventory) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kExtremeLongPosition);

  // This test verifies clamping math (ask > bid at extreme skew), not the
  // price-range gate — open the band to the full valid range so the clamped
  // quotes are placed.
  constexpr int kWidestBandMin = 1;
  constexpr int kWidestBandMax = 99;
  kalshi::RiskLimits limits;
  limits.min_quote_price_cents = kWidestBandMin;
  limits.max_quote_price_cents = kWidestBandMax;
  kalshi::RiskManager risk_mgr{limits};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // Extreme long inventory: the cap suppresses the bid entirely; only the
  // unwind ask quotes, and it still clears the bid range.
  ASSERT_EQ(transport.recorded_requests().size(), 1U);
  const std::string &ask_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(ask_body.find("\"side\":\"ask\""), std::string::npos);
  EXPECT_NE(ask_body.find(std::string(kAskPriceExtremeClamp)),
            std::string::npos);
}

TEST_F(QuoterTest, BidClampedToStayPassiveBelowMarketAsk) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::No,
                       kExtremeLongPosition);

  constexpr int kWidestBandMin = 1;
  constexpr int kWidestBandMax = 99;
  kalshi::RiskLimits limits;
  limits.min_quote_price_cents = kWidestBandMin;
  limits.max_quote_price_cents = kWidestBandMax;
  kalshi::RiskManager risk_mgr{limits};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_FALSE(transport.recorded_requests().empty());
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(R"("price":"0.5200")"), std::string::npos);
  EXPECT_EQ(bid_body.find(R"("price":"0.9800")"), std::string::npos);
}

TEST_F(QuoterTest, ImbalancedFlowWidensSpread) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  // Heavy one-sided flow → imbalanced (30 vs 5, ratio 6 > 2, vol 35 ≥ 20).
  kalshi::FlowImbalanceGuard flow_guard{kalshi::FlowImbalanceConfig{}};
  flow_guard.record_fill(kTicker, kalshi::Side::Yes, kImbalanceYesQty);
  flow_guard.record_fill(kTicker, kalshi::Side::No, kImbalanceNoQty);
  ASSERT_TRUE(flow_guard.is_imbalanced(kTicker));

  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr,
                        &flow_guard};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // mid 52

  // Spread 4 → bid 50 normally; +2 imbalance spread → half 3 → bid 49.
  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceImbalanced)), std::string::npos);
}

TEST_F(QuoterTest, SpreadFloorWidensTooTightTarget) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  // Target spread 2 would quote bid 51, but the min-spread floor of 8 forces
  // it.
  kalshi::QuoterConfig config;
  config.target_spread_cents = kLowTargetSpread;
  config.min_spread_cents = kHighMinSpread;
  kalshi::Quoter quoter{config, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // mid 52

  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceFloored)), std::string::npos);
}

TEST_F(QuoterTest, OddSpreadFloorRoundsHalfSpreadUp) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  kalshi::QuoterConfig config;
  config.target_spread_cents = kLowTargetSpread;
  config.min_spread_cents = kOddMinSpread;
  kalshi::Quoter quoter{config, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceOddFloored)), std::string::npos)
      << "min_spread 3 truncated to half-spread 1 quotes a 2c spread, "
         "violating the floor";
}

TEST_F(QuoterTest, MakerFeeWidensSpread) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};

  // Default spread 4 → bid 50; a 2c maker fee at fv 52 widens it → bid 48.
  kalshi::QuoterConfig config;
  config.maker_fee_rate = kMakerFeeRate;
  kalshi::Quoter quoter{config, order_mgr, risk_mgr};
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // mid 52

  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceFloored)), std::string::npos);
}

TEST_F(QuoterTest, UpdateEmitsQuoteDecisionAnalyticsEvent) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};
  std::vector<std::string> lines;
  kalshi::AnalyticsLogger analytics{
      [&lines](const std::string &line) { lines.push_back(line); }};
  quoter.set_analytics(&analytics);

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_EQ(lines.size(), 1U);
  EXPECT_NE(lines.front().find(R"("type":"quote")"), std::string::npos);
  EXPECT_NE(lines.front().find(R"("ticker":"KXBTCD")"), std::string::npos);
  EXPECT_NE(lines.front().find(R"("bid":50)"), std::string::npos);
  EXPECT_NE(lines.front().find(R"("ask":54)"), std::string::npos);
}

TEST_F(QuoterTest, LmsrSkewZeroInventoryLeavesFairValueUnchanged) {
  const double b_inv =
      kalshi::lmsr_b_from_risk(kalshi::RiskLimits::kDefaultMaxPosition,
                               kalshi::RiskLimits::kDefaultMaxQuotePrice);

  EXPECT_DOUBLE_EQ(kalshi::lmsr_skewed_fair_value(52.0, 0.0, b_inv), 52.0);
}

TEST_F(QuoterTest, LmsrSkewAtMaxPositionReachesQuoteBandEdge) {
  constexpr int kMaxPositionContracts = 100;
  constexpr int kUpperBandCents = 90;
  constexpr double kMidFairValue = 50.0;
  const double b_inv =
      kalshi::lmsr_b_from_risk(kMaxPositionContracts, kUpperBandCents);

  EXPECT_NEAR(kalshi::lmsr_skewed_fair_value(kMidFairValue,
                                             -kMaxPositionContracts, b_inv),
              90.0, 1e-9)
      << "max short must move the reservation price to the upper band edge";
  EXPECT_NEAR(kalshi::lmsr_skewed_fair_value(kMidFairValue,
                                             kMaxPositionContracts, b_inv),
              10.0, 1e-9)
      << "max long must move the reservation price symmetrically down";
}

TEST_F(QuoterTest, LmsrSkewNeverLeavesValidPriceRange) {
  constexpr double kExtremeInventory = 100'000.0;
  const double b_inv =
      kalshi::lmsr_b_from_risk(kalshi::RiskLimits::kDefaultMaxPosition,
                               kalshi::RiskLimits::kDefaultMaxQuotePrice);

  const double marked_down =
      kalshi::lmsr_skewed_fair_value(52.0, kExtremeInventory, b_inv);
  const double marked_up =
      kalshi::lmsr_skewed_fair_value(52.0, -kExtremeInventory, b_inv);

  EXPECT_GE(marked_down, 0.0);
  EXPECT_LT(marked_down, 52.0);
  EXPECT_GT(marked_up, 52.0);
  EXPECT_LE(marked_up, 100.0);
}

TEST_F(QuoterTest, LmsrDegenerateBandDisablesSkew) {
  constexpr int kDegenerateUpperBand = 50;
  const double b_inv = kalshi::lmsr_b_from_risk(
      kalshi::RiskLimits::kDefaultMaxPosition, kDegenerateUpperBand);

  EXPECT_DOUBLE_EQ(kalshi::lmsr_skewed_fair_value(52.0, 40.0, b_inv), 52.0);
}

TEST_F(QuoterTest, LongshotBidShadedByExtraEdge) {
  constexpr int kYesBid30 = 29;
  constexpr int kNoBid30 = 69;
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid30, kNoBid30));

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find("\"side\":\"bid\""), std::string::npos);
  EXPECT_NE(bid_body.find(R"("price":"0.2700")"), std::string::npos)
      << "buying YES below the longshot threshold must cost 1c extra edge";
  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find("\"side\":\"ask\""), std::string::npos);
  EXPECT_NE(ask_body.find(R"("price":"0.3200")"), std::string::npos)
      << "the favorite-side ask quotes normally";
}

TEST_F(QuoterTest, LongshotAskShadedByExtraEdge) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid70, kNoBid70));

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(R"("price":"0.6800")"), std::string::npos)
      << "the favorite-side bid quotes normally";
  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find(R"("price":"0.7300")"), std::string::npos)
      << "buying NO below the longshot threshold must cost 1c extra edge";
}

TEST_F(QuoterTest, MidRangeQuotesNotShadedByLongshotFloor) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceMid52)), std::string::npos);
  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(ask_body.find(std::string(kAskPriceMid52)), std::string::npos);
}

TEST_F(QuoterTest, SingleFillNeverQuotesAGuaranteedLossExit) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId3, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kalshi::QuoterConfig::kDefaultQuoteSize);
  quoter.forget_order(kTicker, kOrderId1);
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  bool checked_ask = false;
  for (const auto &request : transport.recorded_requests()) {
    if (request.body.find("\"side\":\"ask\"") == std::string::npos) {
      continue;
    }
    const auto pos = request.body.find("\"price\":\"0.");
    ASSERT_NE(pos, std::string::npos);
    const int yes_cents = std::stoi(request.body.substr(pos + 11, 2));
    EXPECT_GE(yes_cents, 51)
        << "after buying at 50 the offer must stay above entry — skew biases "
           "flow, it must never quote a locked-in loss";
    checked_ask = true;
  }
  EXPECT_TRUE(checked_ask);
}

TEST_F(QuoterTest, ImbalancedFlowLeansFairValueTowardTakers) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::FlowImbalanceGuard guard{kalshi::FlowImbalanceConfig{}};
  guard.record_fill(kTicker, kalshi::Side::No, kImbalanceYesQty,
                    std::chrono::system_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, &guard};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  // We filled NO 30 lots → takers bought YES → lean +1c. Base imbalanced
  // quote is bid 49 (half 3); with the lean it becomes 50.
  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(R"("price":"0.5000")"), std::string::npos)
      << "persistent one-sided flow must shift the belief toward the flow";
}

TEST_F(QuoterTest, InventoryCapStopsTheAccumulatingSide) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  constexpr int kAtCap = 2 * kalshi::QuoterConfig::kDefaultQuoteSize;
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes, kAtCap);
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_EQ(transport.recorded_requests().size(), 1U)
      << "at 2x quote_size long, only the unwind side may quote";
  EXPECT_NE(transport.recorded_requests().front().body.find("\"side\":\"ask\""),
            std::string::npos);
}

TEST_F(QuoterTest, ClearingModelLeansQuotesTowardTapePrints) {
  constexpr double kClearingHalfWeight = 0.5;
  constexpr int kPrintPrice = 56;
  constexpr int kPrintContracts = 10;
  constexpr std::string_view kBlendedBid = R"("price":"0.5200")";
  constexpr std::string_view kBlendedAskYesLeg = R"("price":"0.5600")";
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{
      kalshi::QuoterConfig{},
      kalshi::FairValueEngine{
          std::make_unique<kalshi::ClearingPriceModel>(kClearingHalfWeight)},
      order_mgr, risk_mgr};

  kalshi::TradeTape tape{kalshi::TradeTapeConfig{}};
  kalshi::PublicTrade print;
  print.trade_id = "pub-1";
  print.market_ticker = kTicker;
  print.yes_price_cents = kPrintPrice;
  print.quantity = kalshi::Quantity::from_contracts(kPrintContracts);
  print.taker_side = kalshi::Side::Yes;
  print.timestamp = std::chrono::system_clock::now();
  tape.record_trade(print);
  quoter.set_trade_tape(&tape);

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  EXPECT_NE(transport.recorded_requests()[0].body.find(kBlendedBid),
            std::string::npos)
      << transport.recorded_requests()[0].body;
  EXPECT_NE(transport.recorded_requests()[1].body.find(kBlendedAskYesLeg),
            std::string::npos)
      << transport.recorded_requests()[1].body;
}

TEST_F(QuoterTest, ForgetTickerRequotesBothSidesAfterOutOfBandCancel) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  for (const auto &order_id : {kOrderId1, kOrderId2, kOrderId3, kOrderId4}) {
    transport.enqueue(
        {kHttpOk,
         order_json(order_id, kalshi::QuoterConfig::kDefaultQuoteSize)});
  }
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  ASSERT_EQ(transport.recorded_requests().size(), 2U);

  quoter.forget_ticker(kTicker);
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_EQ(transport.recorded_requests().size(), 4U)
      << "forgotten ticker must re-place both sides instead of believing its "
         "cancelled orders still rest";
  for (const auto &request : transport.recorded_requests()) {
    EXPECT_EQ(request.method, "POST")
        << "forget is bookkeeping only — the orders were cancelled "
           "out-of-band";
  }
}

TEST_F(QuoterTest, DriftLeanShiftsQuotesWhenSignificantAndEnabled) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::QuoterConfig config;
  constexpr double kDriftGain = 0.25;
  config.drift_lean_gain = kDriftGain;
  kalshi::Quoter quoter{config, order_mgr, risk_mgr};

  kalshi::DriftEstimator drift_estimator{kalshi::DriftEstimatorConfig{}};
  constexpr int kWarmSamples = 13;
  constexpr int kWarmSpacingSeconds = 5;
  constexpr double kWarmSlopeCentsPerSecond = 0.1;
  const auto now = std::chrono::system_clock::now();
  for (int index = 0; index < kWarmSamples; ++index) {
    const int seconds_ago = (kWarmSamples - 1 - index) * kWarmSpacingSeconds;
    drift_estimator.add_sample(kTicker, now - std::chrono::seconds{seconds_ago},
                               52.0 - kWarmSlopeCentsPerSecond * seconds_ago);
  }
  quoter.set_drift_estimator(&drift_estimator);

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  const std::string &ask_body = transport.recorded_requests().at(1).body;
  EXPECT_NE(bid_body.find(R"("price":"0.5100")"), std::string::npos)
      << bid_body;
  EXPECT_NE(ask_body.find(R"("price":"0.5600")"), std::string::npos)
      << ask_body;
}

TEST_F(QuoterTest, DriftLeanOffByDefaultLeavesQuotesUnchanged) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  kalshi::DriftEstimator drift_estimator{kalshi::DriftEstimatorConfig{}};
  constexpr int kWarmSamples = 13;
  constexpr int kWarmSpacingSeconds = 5;
  constexpr double kWarmSlopeCentsPerSecond = 0.1;
  const auto now = std::chrono::system_clock::now();
  for (int index = 0; index < kWarmSamples; ++index) {
    const int seconds_ago = (kWarmSamples - 1 - index) * kWarmSpacingSeconds;
    drift_estimator.add_sample(kTicker, now - std::chrono::seconds{seconds_ago},
                               52.0 - kWarmSlopeCentsPerSecond * seconds_ago);
  }
  quoter.set_drift_estimator(&drift_estimator);

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(std::string(kBidPriceMid52)), std::string::npos);
}

TEST_F(QuoterTest, MakerFeeCeilsPerOrderSoSmallFeesStillWiden) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::QuoterConfig fee_config;
  constexpr double kSmallMakerFeeRate = 0.0175;
  fee_config.maker_fee_rate = kSmallMakerFeeRate;
  kalshi::Quoter quoter{fee_config, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  const std::string &bid_body = transport.recorded_requests().at(0).body;
  EXPECT_NE(bid_body.find(R"("price":"0.4900")"), std::string::npos)
      << "per-order fee ceil must widen by 1c even when the per-contract "
         "fee rounds to zero: "
      << bid_body;
}
