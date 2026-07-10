#include "fv_backtest.hpp"

#include "types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace {

const std::string kTicker = "KXFV";

constexpr int kBidPrice = 60;
constexpr int kNoBidPrice = 36;
constexpr kalshi::Quantity kTenLots = kalshi::Quantity::from_contracts(10);
constexpr kalshi::Quantity kFiveLots = kalshi::Quantity::from_contracts(5);
constexpr int kFirstPrint = 63;
constexpr int kSecondPrint = 65;
constexpr int kSecondPrintGapSeconds = 10;
constexpr double kAnchorMae = 2.0;
constexpr double kAnchorBias = -2.0;
constexpr double kBlendMae = 1.75;
constexpr double kBlendBias = -1.75;
constexpr double kTolerance = 1e-9;
constexpr double kHalfTapeWeight = 0.5;
constexpr int kHalfLifeSeconds = 30;

kalshi::FvBacktestConfig small_config() {
  kalshi::FvBacktestConfig config;
  config.anchors = {
      kalshi::AnchorSpec{.label = "micro", .use_micro = true},
      kalshi::AnchorSpec{.label = "clearing", .use_micro = false},
  };
  config.tape_weights = {0.0, kHalfTapeWeight};
  config.tape_half_life_seconds = {kHalfLifeSeconds};
  config.own_fill_weights = {0.0};
  config.score_horizon_seconds = 0;
  return config;
}

kalshi::FvBacktestConfig horizon_config(int horizon_seconds) {
  kalshi::FvBacktestConfig config;
  config.anchors = {kalshi::AnchorSpec{.label = "micro", .use_micro = true}};
  config.tape_weights = {0.0};
  config.tape_half_life_seconds = {kHalfLifeSeconds};
  config.own_fill_weights = {0.0};
  config.score_horizon_seconds = horizon_seconds;
  return config;
}

kalshi::FvBacktest::TimePoint base_time() {
  constexpr long long kBaseEpochSeconds = 1'700'000'000;
  return kalshi::FvBacktest::TimePoint{} +
         std::chrono::seconds{kBaseEpochSeconds};
}

kalshi::Orderbook make_book() {
  kalshi::Orderbook book;
  book.ticker = kTicker;
  book.yes = {{kBidPrice, kTenLots}};
  book.no = {{kNoBidPrice, kTenLots}};
  return book;
}

kalshi::PublicTrade make_trade(const std::string &trade_id, int price,
                               kalshi::FvBacktest::TimePoint when) {
  kalshi::PublicTrade trade;
  trade.trade_id = trade_id;
  trade.market_ticker = kTicker;
  trade.yes_price_cents = price;
  trade.quantity = kFiveLots;
  trade.taker_side = kalshi::Side::Yes;
  trade.timestamp = when;
  return trade;
}

const kalshi::FvScore &find_score(const std::vector<kalshi::FvScore> &scores,
                                  const std::string &name) {
  const auto found = std::find_if(scores.begin(), scores.end(),
                                  [&name](const kalshi::FvScore &score) {
                                    return score.candidate == name;
                                  });
  EXPECT_NE(found, scores.end()) << "missing candidate: " << name;
  return *found;
}

} // namespace

TEST(FvBacktestTest, AnchorsScoredAgainstEachPrint) {
  kalshi::FvBacktest backtest{small_config()};
  backtest.on_snapshot(make_book());
  backtest.on_trade(make_trade("t1", kFirstPrint, base_time()));
  backtest.on_trade(
      make_trade("t2", kSecondPrint,
                 base_time() + std::chrono::seconds{kSecondPrintGapSeconds}));

  const auto scores = backtest.scores();
  const auto &micro = find_score(scores, "micro");
  EXPECT_EQ(micro.events, 2);
  EXPECT_NEAR(micro.mae_cents, kAnchorMae, kTolerance);
  EXPECT_NEAR(micro.bias_cents, kAnchorBias, kTolerance);
  const auto &clearing = find_score(scores, "clearing");
  EXPECT_EQ(clearing.events, 2);
  EXPECT_NEAR(clearing.mae_cents, kAnchorMae, kTolerance);
}

TEST(FvBacktestTest, TapeBlendUsesEarlierPrintsOnly) {
  kalshi::FvBacktest backtest{small_config()};
  backtest.on_snapshot(make_book());
  backtest.on_trade(make_trade("t1", kFirstPrint, base_time()));
  backtest.on_trade(
      make_trade("t2", kSecondPrint,
                 base_time() + std::chrono::seconds{kSecondPrintGapSeconds}));

  const auto scores = backtest.scores();
  const auto &blend = find_score(scores, "micro+tape(w=0.5,h=30s)");
  EXPECT_EQ(blend.events, 2);
  EXPECT_NEAR(blend.mae_cents, kBlendMae, kTolerance);
  EXPECT_NEAR(blend.bias_cents, kBlendBias, kTolerance);
}

TEST(FvBacktestTest, OwnFillPrintsExcludedFromTapeButStillScored) {
  kalshi::FvBacktest backtest{small_config()};
  backtest.on_snapshot(make_book());
  kalshi::Fill own_fill;
  own_fill.trade_id = "mine";
  own_fill.market_ticker = kTicker;
  backtest.on_fill(own_fill);
  backtest.on_trade(make_trade("mine", kFirstPrint, base_time()));
  backtest.on_trade(
      make_trade("t2", kSecondPrint,
                 base_time() + std::chrono::seconds{kSecondPrintGapSeconds}));

  const auto scores = backtest.scores();
  const auto &blend = find_score(scores, "micro+tape(w=0.5,h=30s)");
  EXPECT_EQ(blend.events, 2);
  EXPECT_NEAR(blend.mae_cents, kAnchorMae, kTolerance);
  EXPECT_NEAR(blend.bias_cents, kAnchorBias, kTolerance);
}

TEST(FvBacktestTest, PrintsWithoutABookAreNotScored) {
  kalshi::FvBacktest backtest{small_config()};
  backtest.on_trade(make_trade("t1", kFirstPrint, base_time()));

  for (const auto &score : backtest.scores()) {
    EXPECT_EQ(score.events, 0);
  }
}

TEST(FvBacktestTest, DeltasMoveTheAnchor) {
  kalshi::FvBacktest backtest{small_config()};
  backtest.on_snapshot(make_book());
  backtest.on_delta(kTicker, kalshi::Side::Yes, kBidPrice, -kTenLots);
  constexpr int kNewBid = 62;
  backtest.on_delta(kTicker, kalshi::Side::Yes, kNewBid, kTenLots);
  constexpr int kNewMicro = 63;
  backtest.on_trade(make_trade("t1", kNewMicro, base_time()));

  const auto scores = backtest.scores();
  const auto &micro = find_score(scores, "micro");
  EXPECT_EQ(micro.events, 1);
  EXPECT_NEAR(micro.mae_cents, 0.0, kTolerance);
}

TEST(FvBacktestTest, ScoresSortedByMaeAscending) {
  kalshi::FvBacktest backtest{small_config()};
  backtest.on_snapshot(make_book());
  backtest.on_trade(make_trade("t1", kFirstPrint, base_time()));
  backtest.on_trade(
      make_trade("t2", kSecondPrint,
                 base_time() + std::chrono::seconds{kSecondPrintGapSeconds}));

  const auto scores = backtest.scores();
  for (std::size_t idx = 1; idx < scores.size(); ++idx) {
    EXPECT_LE(scores[idx - 1].mae_cents, scores[idx].mae_cents);
  }
}

TEST(FvBacktestTest, HorizonGradesAgainstFirstPrintAtOrAfterDeadline) {
  constexpr int kHorizonSeconds = 30;
  constexpr int kEarlyGapSeconds = 10;
  constexpr int kLateGapSeconds = 40;
  constexpr int kMidPrint = 64;
  constexpr int kLatePrint = 65;
  constexpr double kHorizonMae = 3.0;
  kalshi::FvBacktest backtest{horizon_config(kHorizonSeconds)};
  backtest.on_snapshot(make_book());
  backtest.on_trade(make_trade("t1", kFirstPrint, base_time()));
  backtest.on_trade(make_trade(
      "t2", kMidPrint, base_time() + std::chrono::seconds{kEarlyGapSeconds}));
  backtest.on_trade(make_trade(
      "t3", kLatePrint, base_time() + std::chrono::seconds{kLateGapSeconds}));

  const auto scores = backtest.scores();
  const auto &micro = find_score(scores, "micro");
  EXPECT_EQ(micro.events, 2);
  EXPECT_NEAR(micro.mae_cents, kHorizonMae, kTolerance);
  EXPECT_NEAR(micro.bias_cents, -kHorizonMae, kTolerance);
}

TEST(FvBacktestTest, HorizonZeroGradesAgainstTheArrivingPrint) {
  kalshi::FvBacktest backtest{horizon_config(0)};
  backtest.on_snapshot(make_book());
  backtest.on_trade(make_trade("t1", kFirstPrint, base_time()));
  backtest.on_trade(
      make_trade("t2", kSecondPrint,
                 base_time() + std::chrono::seconds{kSecondPrintGapSeconds}));

  const auto scores = backtest.scores();
  const auto &micro = find_score(scores, "micro");
  EXPECT_EQ(micro.events, 2);
  EXPECT_NEAR(micro.mae_cents, kAnchorMae, kTolerance);
  EXPECT_NEAR(micro.bias_cents, kAnchorBias, kTolerance);
}

TEST(FvBacktestTest, HorizonPendingEventsDoNotCrossTickers) {
  constexpr int kHorizonSeconds = 30;
  constexpr int kOtherPrint = 50;
  constexpr int kOtherGapSeconds = 35;
  constexpr int kResolveGapSeconds = 40;
  constexpr int kLatePrint = 65;
  constexpr double kHorizonMae = 3.0;
  const std::string kOtherTicker = "KXOTHER";
  kalshi::FvBacktest backtest{horizon_config(kHorizonSeconds)};
  backtest.on_snapshot(make_book());
  kalshi::Orderbook other_book = make_book();
  other_book.ticker = kOtherTicker;
  backtest.on_snapshot(other_book);

  backtest.on_trade(make_trade("t1", kFirstPrint, base_time()));
  kalshi::PublicTrade other = make_trade(
      "o1", kOtherPrint, base_time() + std::chrono::seconds{kOtherGapSeconds});
  other.market_ticker = kOtherTicker;
  backtest.on_trade(other);
  backtest.on_trade(
      make_trade("t3", kLatePrint,
                 base_time() + std::chrono::seconds{kResolveGapSeconds}));

  const auto scores = backtest.scores();
  const auto &micro = find_score(scores, "micro");
  EXPECT_EQ(micro.events, 1);
  EXPECT_NEAR(micro.mae_cents, kHorizonMae, kTolerance);
}

TEST(FvBacktestTest, DefaultConfigGeneratesGridWithMicroAndClearing) {
  const auto config = kalshi::FvBacktestConfig::defaults();
  kalshi::FvBacktest backtest{config};
  const auto scores = backtest.scores();
  EXPECT_GT(scores.size(), config.anchors.size());
  const auto has = [&scores](const std::string &name) {
    return std::any_of(scores.begin(), scores.end(),
                       [&name](const kalshi::FvScore &score) {
                         return score.candidate == name;
                       });
  };
  EXPECT_TRUE(has("micro"));
  EXPECT_TRUE(has("clearing(flat)"));
}
