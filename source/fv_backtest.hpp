#pragma once

// Tick-scale fair-value backtest over replayed capture sessions
// (BETTER_PRICING.md Phase 3b). Consumes the same event stream the live
// session sees — book snapshots/deltas, public trades, own fills — through
// the production LocalOrderbook and TradeTape, and grades a grid of fv
// candidates (micro / clearing-price anchors × tape blend weights ×
// half-lives × own-fill weights) against each public print: MAE = accuracy,
// bias = the signed lean that becomes drift against held inventory. Own
// fills still count as scoring targets (a taker crossed at that price);
// they are only excluded from the tape per own_fill_weight.

#include "orderbook.hpp"
#include "trade_tape.hpp"
#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace kalshi {

struct AnchorSpec {
  std::string label;
  bool use_micro{false};
  DepthWeighting weighting{};
};

struct FvBacktestConfig {
  static constexpr int kDefaultTapeWindowSeconds = 600;

  std::vector<AnchorSpec> anchors;
  std::vector<double> tape_weights;
  std::vector<int> tape_half_life_seconds;
  std::vector<double> own_fill_weights;
  int tape_window_seconds = kDefaultTapeWindowSeconds;

  [[nodiscard]] static FvBacktestConfig defaults();
};

struct FvScore {
  std::string candidate;
  int events{0};
  double mae_cents{0.0};
  double bias_cents{0.0};
};

class FvBacktest {
public:
  using Clock = std::chrono::system_clock;
  using TimePoint = Clock::time_point;

  explicit FvBacktest(FvBacktestConfig config);

  void on_snapshot(const Orderbook &snapshot);
  void on_delta(const std::string &ticker, Side side, int price_cents,
                Quantity delta);
  void on_trade(const PublicTrade &trade);
  void on_fill(const Fill &fill);

  [[nodiscard]] std::vector<FvScore> scores() const;

private:
  struct Candidate {
    std::string name;
    std::size_t anchor_index{0};
    double tape_weight{0.0};
    std::chrono::seconds half_life{0};
    std::size_t tape_index{0};
  };
  struct Accumulator {
    int events{0};
    double abs_error_sum{0.0};
    double signed_error_sum{0.0};
  };

  void build_candidates();

  FvBacktestConfig config_;
  std::vector<Candidate> candidates_;
  std::vector<Accumulator> accumulators_;
  std::vector<TradeTape> tapes_;
  std::unordered_map<std::string, LocalOrderbook> books_;
};

} // namespace kalshi
