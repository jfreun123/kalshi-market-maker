#include "quoter_test_util.hpp"

// ---- Tests ----

TEST_F(QuoterTest, QuoteNotReplacedWithinRepriceThreshold) {
  // First update (mid=52): bid=50, ask=54.
  // Second update (mid=53): desired bid=51, ask=55; diff=1 = threshold → no
  // replace.
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
  quoter.update(kTicker, make_ob(kYesBid53, kNoBid53)); // diff=1, no reprice

  EXPECT_EQ(transport.recorded_requests().size(), 2U);
}

TEST_F(QuoterTest, QuoteReplacedWhenExceedingRepriceThreshold) {
  // First update (mid=52): bid=50, ask=54.
  // Second update (mid=54): bid=52, ask=56; diff=2 > 1 (and below the fade
  // trigger) → plain cancel + replace on both sides.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  // Second update: one atomic amend per side (no cancel+replace pair).
  transport.enqueue({kHttpOk, amend_json(kOrderId1)});
  transport.enqueue({kHttpOk, amend_json(kOrderId2)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::QuoterConfig unsmoothed;
  unsmoothed.fv_ema_alpha = 1.0;
  kalshi::Quoter quoter{unsmoothed, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  *clock_now += kMinRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid54, kNoBid54));

  // 2 POSTs (place) + 2 POSTs (amend) = 4 total requests.
  EXPECT_EQ(transport.recorded_requests().size(), 4U);
  EXPECT_NE(transport.recorded_requests().at(2).url.find("/amend"),
            std::string::npos);
}

TEST_F(QuoterTest, RepriceSuppressedWhileQuoteYoungerThanMinRest) {
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
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  *clock_now += kJustUnderMinRest;
  quoter.update(kTicker, make_ob(kYesBid54, kNoBid54));

  EXPECT_EQ(transport.recorded_requests().size(), 2U)
      << "a sub-theo-jump move must not cancel a quote that has rested "
         "< min_rest_ms";
}

TEST_F(QuoterTest, RepriceResumesOnceQuoteHasRestedMinRest) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, amend_json(kOrderId1)});
  transport.enqueue({kHttpOk, amend_json(kOrderId2)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::QuoterConfig unsmoothed;
  unsmoothed.fv_ema_alpha = 1.0;
  kalshi::Quoter quoter{unsmoothed, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  *clock_now += kMinRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid54, kNoBid54));

  EXPECT_EQ(transport.recorded_requests().size(), 4U)
      << "after resting min_rest_ms the quote must reprice (via amend)";
}

TEST_F(QuoterTest, RepriceIgnoresOwnRestingQuotesEchoedInBook) {
  // Thin book (both sides 1 lot): mid = micro = 35 → bid=33, longshot-shaded
  // to 32; ask=37 (NO 63). The exchange then echoes our resting 10-lot bid at
  // 32 back as the best bid. Priced off the raw echo, micro jumps and
  // the quoter chases its own order even past min_rest_ms — the
  // self-referential churn oscillator (finding D4). Priced off the book minus
  // our own orders, fair value stays 35 and both resting quotes are left
  // alone. No fill is involved, so inventory stays zero and the LMSR skew is
  // inert — this isolates the own-quote subtraction.
  constexpr int kThinYesBid = 30;
  constexpr int kThinNoBid = 60;
  constexpr int kOwnBidPrice = 32;
  const kalshi::Quantity kThinQty = kalshi::Quantity::from_contracts(1);
  const kalshi::Quantity kOwnQty =
      kalshi::Quantity::from_contracts(kalshi::QuoterConfig::kDefaultQuoteSize);

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
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  kalshi::Orderbook thin;
  thin.ticker = kTicker;
  thin.yes = {{kThinYesBid, kThinQty}};
  thin.no = {{kThinNoBid, kThinQty}};
  kalshi::LocalOrderbook book;
  book.apply_snapshot(thin);
  quoter.update(kTicker, book);
  ASSERT_EQ(transport.recorded_requests().size(), 2U);

  *clock_now += kMinRestElapsed;
  kalshi::Orderbook echo;
  echo.ticker = kTicker;
  echo.yes = {{kThinYesBid, kThinQty}, {kOwnBidPrice, kOwnQty}};
  echo.no = {{kThinNoBid, kThinQty}};
  kalshi::LocalOrderbook echo_book;
  echo_book.apply_snapshot(echo);
  quoter.update(kTicker, echo_book);

  EXPECT_EQ(transport.recorded_requests().size(), 2U)
      << "the echoed own bid must not move either quote even after min_rest";
}

TEST_F(QuoterTest, ResetQuotesForgetsLiveStateSoNextUpdatePlacesFresh) {
  // After an out-of-band flatten (risk halt / disconnect cancels the resting
  // orders), the quoter must be told to forget them. Otherwise it believes its
  // quotes are still live, tries to cancel dead ids, and never re-quotes.
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
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // 2 POSTs (bid + ask)
  ASSERT_EQ(transport.recorded_requests().size(), 2U);

  quoter.reset_quotes();

  // Same mid: without the reset the quoter would think it is already quoting at
  // the right price and do nothing; after the reset it places a fresh pair.
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  EXPECT_EQ(transport.recorded_requests().size(), 4U);
  EXPECT_EQ(transport.recorded_requests().at(2).method, "POST");
  EXPECT_EQ(transport.recorded_requests().at(3).method, "POST");
}

TEST_F(QuoterTest, SelfCrossGuardSkipsBidWhenItWouldCrossOwnAsk) {
  // First update (mid=52): bid=50, ask=54 (current_ask_cents=54).
  // Second update (mid=70): desired bid=68 >= current_ask 54 → bid suppressed.
  // Only ask is repriced: DELETE ask, POST ask@72. Bid stays cancelled.
  // Expected requests: POST bid, POST ask, DELETE bid, DELETE ask, POST ask
  // = 5.
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  // Initial: POST bid order-001, POST ask order-002.
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  // Reprice: DELETE bid order-001 (would cross own ask — no amend, no
  // re-entry). Ask reprice: one amend.
  transport.enqueue(
      {kHttpOk, R"({"order_id":"order-001","reduced_by":"5.00","ts_ms":0})"});
  transport.enqueue({kHttpOk, amend_json(kOrderId2)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::QuoterConfig unsmoothed;
  unsmoothed.fv_ema_alpha = 1.0;
  kalshi::Quoter quoter{unsmoothed, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52)); // bid=50, ask=54
  *clock_now += kMinRestElapsed;
  quoter.update(kTicker,
                make_ob(kYesBid70, kNoBid70)); // desired bid=68 crosses ask
  *clock_now += kFadeRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid70, kNoBid70)); // confirm ask fade

  // POST bid + POST ask + DELETE bid + amend ask = 4
  EXPECT_EQ(transport.recorded_requests().size(), 4U);
  const std::string &last_body = transport.recorded_requests().back().body;
  EXPECT_NE(last_body.find("\"side\":\"ask\""), std::string::npos);
  EXPECT_NE(transport.recorded_requests().back().url.find("/amend"),
            std::string::npos);
}

TEST_F(QuoterTest, BidFadesOnAdverseTheoDropDespiteMinRest) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, amend_json(kOrderId3)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::QuoterConfig unsmoothed;
  unsmoothed.fv_ema_alpha = 1.0;
  kalshi::Quoter quoter{unsmoothed, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  *clock_now += kFadeRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid47, kNoBid47));
  *clock_now += kFadeRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid47, kNoBid47));

  EXPECT_EQ(transport.recorded_requests().size(), 3U)
      << "the toxic bid must fade via one amend; the safe ask must keep "
         "resting under min_rest_ms";
  const std::string &amended_bid = transport.recorded_requests().at(2).body;
  EXPECT_NE(amended_bid.find("\"side\":\"bid\""), std::string::npos);
}

TEST_F(QuoterTest, AskFadesOnAdverseTheoRiseDespiteMinRest) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, amend_json(kOrderId3)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::QuoterConfig unsmoothed;
  unsmoothed.fv_ema_alpha = 1.0;
  kalshi::Quoter quoter{unsmoothed, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  *clock_now += kFadeRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid57, kNoBid57));
  *clock_now += kFadeRestElapsed;
  quoter.update(kTicker, make_ob(kYesBid57, kNoBid57));

  EXPECT_EQ(transport.recorded_requests().size(), 3U)
      << "the toxic ask must fade via one amend; the safe bid must keep "
         "resting under min_rest_ms";
  const std::string &amended_ask = transport.recorded_requests().at(2).body;
  EXPECT_NE(amended_ask.find("\"side\":\"ask\""), std::string::npos);
}

TEST_F(QuoterTest, AdverseTheoJumpStillHeldUnderFadeRestFloor) {
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
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::QuoterConfig unsmoothed;
  unsmoothed.fv_ema_alpha = 1.0;
  kalshi::Quoter quoter{unsmoothed, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  quoter.update(kTicker, make_ob(kYesBid47, kNoBid47));
  *clock_now += kJustUnderFadeRest;
  quoter.update(kTicker, make_ob(kYesBid47, kNoBid47));

  EXPECT_EQ(transport.recorded_requests().size(), 2U)
      << "the sub-second echo window must not trigger fades";
}

TEST_F(QuoterTest, EmaSmoothsFlappingMicroPriceBelowFadeTrigger) {
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
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  for (int flap = 0; flap < 6; ++flap) {
    *clock_now += kFadeRestElapsed;
    quoter.update(kTicker, make_ob(kYesBid55, kNoBid55));
    *clock_now += kFadeRestElapsed;
    quoter.update(kTicker, make_ob(kYesBid49, kNoBid49));
  }

  EXPECT_EQ(transport.recorded_requests().size(), 2U)
      << "a +/-3c flapping micro-price must be smoothed below the fade "
         "trigger — the D13 oscillator";
}

TEST_F(QuoterTest, EmaConvergesOnSustainedMoveAndStillFades) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, "{}"});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId3, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, "{}"});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  for (int tick = 0; tick < 8; ++tick) {
    *clock_now += kFadeRestElapsed;
    quoter.update(kTicker, make_ob(kYesBid47, kNoBid47));
  }

  EXPECT_GE(transport.recorded_requests().size(), 4U)
      << "a sustained 5c drop must converge through the EMA and fade the "
         "toxic bid";
}

TEST_F(QuoterTest, EmaResetsWithResetQuotes) {
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
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  quoter.reset_quotes();
  quoter.update(kTicker, make_ob(kYesBid70, kNoBid70));

  ASSERT_EQ(transport.recorded_requests().size(), 4U);
  const std::string &fresh_bid = transport.recorded_requests().at(2).body;
  EXPECT_NE(fresh_bid.find(R"("price":"0.6800")"), std::string::npos)
      << "after reset the EMA must not drag the new session's fair value "
         "toward the old market level";
}

TEST_F(QuoterTest, ReduceOnlyQuotesOnlyTheUnwindSideWhenLong) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kInventoryPosition);
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};
  quoter.set_reduce_only(true);

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  ASSERT_EQ(transport.recorded_requests().size(), 1U)
      << "long inventory: only the ask (sell side) may quote";
  EXPECT_NE(transport.recorded_requests().front().body.find("\"side\":\"ask\""),
            std::string::npos);
}

TEST_F(QuoterTest, ReduceOnlyPlacesNothingWhenFlat) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};
  quoter.set_reduce_only(true);

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  EXPECT_EQ(transport.recorded_requests().size(), 0U)
      << "flat book in wind-down: do not open new exposure";
}

TEST_F(QuoterTest, ReduceOnlyCancelsTheAccumulatingSide) {
  auto transport_ptr = std::make_unique<FakeTransport>();
  FakeTransport &transport = *transport_ptr;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId1, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId2, kalshi::QuoterConfig::kDefaultQuoteSize)});
  transport.enqueue({kHttpOk, "{}"});
  kalshi::RestClient rest{kalshi::Auth{kApiKey, kPemPrivateKey},
                          std::move(transport_ptr), kBaseUrl};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  ASSERT_EQ(transport.recorded_requests().size(), 2U);
  record_position_fill(order_mgr, kOrderId1, kalshi::Side::Yes,
                       kInventoryPosition);
  quoter.set_reduce_only(true);
  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));

  EXPECT_EQ(transport.recorded_requests().size(), 3U)
      << "the resting bid (which would add to the long) must be cancelled";
  EXPECT_EQ(transport.recorded_requests().back().method, "DELETE");
}

TEST_F(QuoterTest, PanicJumpCancelsToxicSideAndQuietsUntilSettle) {
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
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::QuoterConfig panic_config;
  panic_config.fv_ema_alpha = 1.0;
  constexpr int kPanicJumpCents = 8;
  panic_config.panic_jump_cents = kPanicJumpCents;
  kalshi::Quoter quoter{panic_config, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};
  constexpr int kYesBid42 = 41;
  constexpr int kNoBid42 = 57;

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  transport.enqueue({kHttpOk, "{}"});
  quoter.update(kTicker, make_ob(kYesBid42, kNoBid42));

  EXPECT_EQ(count_method(transport, "DELETE"), 1)
      << "panic jump must cancel the toxic bid instantly, no rest floor";
  EXPECT_EQ(count_side_posts(transport, "bid"), 1)
      << "no immediate re-quote after the panic pull";

  constexpr auto kInsideSettle = std::chrono::milliseconds{1600};
  *clock_now += kInsideSettle;
  transport.enqueue({kHttpOk, amend_json(kOrderId3)});
  quoter.update(kTicker, make_ob(kYesBid42, kNoBid42));
  EXPECT_EQ(count_side_posts(transport, "bid"), 1)
      << "panic side stays quiet inside the settle window";

  constexpr auto kPastSettle = std::chrono::milliseconds{1000};
  *clock_now += kPastSettle;
  transport.enqueue(
      {kHttpOk,
       order_json(kOrderId4, kalshi::QuoterConfig::kDefaultQuoteSize)});
  quoter.update(kTicker, make_ob(kYesBid42, kNoBid42));
  EXPECT_EQ(count_side_posts(transport, "bid"), 2)
      << "book settled — the panicked side re-quotes";
}

TEST_F(QuoterTest, PanicDisabledByDefaultUsesFadeConfirmation) {
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
  auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  kalshi::QuoterConfig unsmoothed;
  unsmoothed.fv_ema_alpha = 1.0;
  kalshi::Quoter quoter{unsmoothed, order_mgr, risk_mgr, nullptr,
                        [clock_now] { return *clock_now; }};
  constexpr int kYesBid42 = 41;
  constexpr int kNoBid42 = 57;

  quoter.update(kTicker, make_ob(kYesBid52, kNoBid52));
  quoter.update(kTicker, make_ob(kYesBid42, kNoBid42));

  EXPECT_EQ(count_method(transport, "DELETE"), 0)
      << "without the panic tier the fade confirmation path applies";
}
