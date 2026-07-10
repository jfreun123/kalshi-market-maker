#include "fv_backtest.hpp"

#include <algorithm>
#include <format>

namespace kalshi {

namespace {

constexpr double kFlatAnchorWeight = 0.0;
constexpr double kLightTapeWeight = 0.1;
constexpr double kQuarterTapeWeight = 0.25;
constexpr double kHalfTapeWeight = 0.5;
constexpr double kHeavyTapeWeight = 0.75;
constexpr double kPureTapeWeight = 1.0;
constexpr int kFastHalfLifeSeconds = 10;
constexpr int kShortHalfLifeSeconds = 30;
constexpr int kLongHalfLifeSeconds = 120;
constexpr double kNoOwnFillCredit = 0.0;
constexpr double kHalfOwnFillCredit = 0.5;
constexpr double kWallDecayPerCent = 0.5;

} // namespace

FvBacktestConfig FvBacktestConfig::defaults() {
  FvBacktestConfig config;
  config.anchors = {
      AnchorSpec{.label = "micro", .use_micro = true},
      AnchorSpec{.label = "clearing(flat)", .use_micro = false},
      AnchorSpec{.label = "clearing(d=0.5)",
                 .use_micro = false,
                 .weighting =
                     DepthWeighting{.decay_per_cent = kWallDecayPerCent}},
  };
  config.tape_weights = {kFlatAnchorWeight,  kLightTapeWeight,
                         kQuarterTapeWeight, kHalfTapeWeight,
                         kHeavyTapeWeight,   kPureTapeWeight};
  config.tape_half_life_seconds = {kFastHalfLifeSeconds, kShortHalfLifeSeconds,
                                   kLongHalfLifeSeconds};
  config.own_fill_weights = {kNoOwnFillCredit, kHalfOwnFillCredit};
  return config;
}

FvBacktest::FvBacktest(FvBacktestConfig config) : config_{std::move(config)} {
  for (const double own_weight : config_.own_fill_weights) {
    TradeTapeConfig tape_config;
    tape_config.window_seconds = config_.tape_window_seconds;
    tape_config.own_fill_weight = own_weight;
    tapes_.emplace_back(tape_config);
  }
  build_candidates();
}

void FvBacktest::Accumulator::record(double error_cents) {
  ++events;
  abs_error_sum += std::abs(error_cents);
  signed_error_sum += error_cents;
}

void FvBacktest::build_candidates() {
  for (std::size_t anchor_index = 0; anchor_index < config_.anchors.size();
       ++anchor_index) {
    const auto &anchor = config_.anchors[anchor_index];
    candidates_.push_back(Candidate{
        .name = anchor.label, .anchor_index = anchor_index, .accumulator = {}});
    for (const double tape_weight : config_.tape_weights) {
      if (tape_weight <= 0.0) {
        continue;
      }
      for (const int half_life : config_.tape_half_life_seconds) {
        for (std::size_t tape_index = 0; tape_index < tapes_.size();
             ++tape_index) {
          const double own_weight = config_.own_fill_weights[tape_index];
          std::string name = std::format("{}+tape(w={:g},h={}s", anchor.label,
                                         tape_weight, half_life);
          if (own_weight > 0.0) {
            name += std::format(",own={:g}", own_weight);
          }
          name += ")";
          candidates_.push_back(
              Candidate{.name = std::move(name),
                        .anchor_index = anchor_index,
                        .tape_weight = tape_weight,
                        .half_life = std::chrono::seconds{half_life},
                        .tape_index = tape_index,
                        .accumulator = {}});
        }
      }
    }
  }
}

void FvBacktest::on_snapshot(const Orderbook &snapshot) {
  books_[snapshot.ticker].apply_snapshot(snapshot);
}

void FvBacktest::on_delta(const std::string &ticker, Side side, int price_cents,
                          Quantity delta) {
  books_[ticker].apply_delta(side, price_cents, delta);
}

void FvBacktest::on_fill(const Fill &fill) {
  for (auto &tape : tapes_) {
    tape.record_own_fill(fill.trade_id);
  }
}

void FvBacktest::on_trade(const PublicTrade &trade) {
  queue_scores(trade);
  resolve_pending(trade);
  for (auto &tape : tapes_) {
    tape.record_trade(trade);
  }
}

void FvBacktest::queue_scores(const PublicTrade &trade) {
  const auto book_it = books_.find(trade.market_ticker);
  if (book_it == books_.end()) {
    return;
  }
  const LocalOrderbook &book = book_it->second;
  if (!book.best_bid().has_value() || !book.best_ask().has_value()) {
    return;
  }
  PendingScore pending;
  pending.deadline =
      trade.timestamp + std::chrono::seconds{config_.score_horizon_seconds};
  pending.fair_values.reserve(candidates_.size());
  for (const Candidate &candidate : candidates_) {
    pending.fair_values.push_back(fair_value_of(candidate, book, trade));
  }
  pending_by_ticker_[trade.market_ticker].push_back(std::move(pending));
}

void FvBacktest::resolve_pending(const PublicTrade &trade) {
  const auto pending_it = pending_by_ticker_.find(trade.market_ticker);
  if (pending_it == pending_by_ticker_.end()) {
    return;
  }
  auto &queue = pending_it->second;
  while (!queue.empty() && queue.front().deadline <= trade.timestamp) {
    const PendingScore &due = queue.front();
    for (std::size_t idx = 0; idx < candidates_.size(); ++idx) {
      candidates_[idx].accumulator.record(due.fair_values[idx] -
                                          trade.yes_price_cents);
    }
    queue.pop_front();
  }
}

double FvBacktest::fair_value_of(const Candidate &candidate,
                                 const LocalOrderbook &book,
                                 const PublicTrade &trade) const {
  const AnchorSpec &anchor = config_.anchors[candidate.anchor_index];
  const double anchor_price = anchor.use_micro
                                  ? book.micro_price_cents()
                                  : book.clearing_price_cents(anchor.weighting);
  if (candidate.tape_weight <= 0.0) {
    return anchor_price;
  }
  const auto tape_vwap = tapes_[candidate.tape_index].vwap_cents(
      trade.market_ticker, candidate.half_life, trade.timestamp);
  if (!tape_vwap.has_value()) {
    return anchor_price;
  }
  return (candidate.tape_weight * *tape_vwap) +
         ((1.0 - candidate.tape_weight) * anchor_price);
}

std::vector<FvScore> FvBacktest::scores() const {
  std::vector<FvScore> results;
  results.reserve(candidates_.size());
  for (const Candidate &candidate : candidates_) {
    const auto &[events, abs_error_sum, signed_error_sum] =
        candidate.accumulator;
    FvScore score;
    score.candidate = candidate.name;
    score.events = events;
    if (events > 0) {
      score.mae_cents = abs_error_sum / events;
      score.bias_cents = signed_error_sum / events;
    }
    results.push_back(std::move(score));
  }
  std::ranges::stable_sort(results,
                           [](const FvScore &left, const FvScore &right) {
                             return left.mae_cents < right.mae_cents;
                           });
  return results;
}

} // namespace kalshi
