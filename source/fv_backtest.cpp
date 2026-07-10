#include "fv_backtest.hpp"

#include <algorithm>
#include <format>

namespace kalshi {

namespace {

constexpr double kFlatAnchorWeight = 0.0;
constexpr double kLightTapeWeight = 0.1;
constexpr double kQuarterTapeWeight = 0.25;
constexpr double kHalfTapeWeight = 0.5;
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
  config.tape_weights = {kFlatAnchorWeight, kLightTapeWeight,
                         kQuarterTapeWeight, kHalfTapeWeight};
  config.tape_half_life_seconds = {kShortHalfLifeSeconds, kLongHalfLifeSeconds};
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
  accumulators_.resize(candidates_.size());
}

void FvBacktest::build_candidates() {
  for (std::size_t anchor_index = 0; anchor_index < config_.anchors.size();
       ++anchor_index) {
    const auto &anchor = config_.anchors[anchor_index];
    candidates_.push_back(
        Candidate{.name = anchor.label, .anchor_index = anchor_index});
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
                        .tape_index = tape_index});
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
  const auto book_it = books_.find(trade.market_ticker);
  if (book_it != books_.end() && book_it->second.best_bid().has_value() &&
      book_it->second.best_ask().has_value()) {
    const LocalOrderbook &book = book_it->second;
    for (std::size_t idx = 0; idx < candidates_.size(); ++idx) {
      const Candidate &candidate = candidates_[idx];
      const AnchorSpec &anchor = config_.anchors[candidate.anchor_index];
      const double anchor_price =
          anchor.use_micro ? book.micro_price_cents()
                           : book.clearing_price_cents(anchor.weighting);
      double fair_value = anchor_price;
      if (candidate.tape_weight > 0.0) {
        const auto vwap = tapes_[candidate.tape_index].vwap_cents(
            trade.market_ticker, candidate.half_life, trade.timestamp);
        if (vwap.has_value()) {
          fair_value = (candidate.tape_weight * *vwap) +
                       ((1.0 - candidate.tape_weight) * anchor_price);
        }
      }
      const double error = fair_value - trade.yes_price_cents;
      Accumulator &accumulator = accumulators_[idx];
      ++accumulator.events;
      accumulator.abs_error_sum += std::abs(error);
      accumulator.signed_error_sum += error;
    }
  }
  for (auto &tape : tapes_) {
    tape.record_trade(trade);
  }
}

std::vector<FvScore> FvBacktest::scores() const {
  std::vector<FvScore> results;
  results.reserve(candidates_.size());
  for (std::size_t idx = 0; idx < candidates_.size(); ++idx) {
    const Accumulator &accumulator = accumulators_[idx];
    FvScore score;
    score.candidate = candidates_[idx].name;
    score.events = accumulator.events;
    if (accumulator.events > 0) {
      score.mae_cents = accumulator.abs_error_sum / accumulator.events;
      score.bias_cents = accumulator.signed_error_sum / accumulator.events;
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
