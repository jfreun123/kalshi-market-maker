#include "quoter.hpp"

#include "ensure.hpp"
#include "flow_imbalance.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace kalshi {

// Time horizon used when the market close time is unknown.
// At 1000 hours the time-decay term is negligible (< 0.01 cents effect).
constexpr double kDefaultTimeToCloseHours = 1'000.0;
constexpr int kBidMinCents = 1;
constexpr int kBidMaxCents = 98;
constexpr int kAskMinCents = 2;
constexpr int kAskMaxCents = 99;
constexpr int kHalfSpreadMin = 1;
constexpr double kContractMaxCents = 100.0;

Quoter::Quoter(QuoterConfig config, IOrderManager &order_mgr,
               RiskManager &risk_mgr, const FlowImbalanceGuard *flow_guard)
    : Quoter(config, FairValueEngine{std::make_unique<HeuristicModel>()},
             order_mgr, risk_mgr, flow_guard) {}

Quoter::Quoter(QuoterConfig config, FairValueEngine fv_engine,
               IOrderManager &order_mgr, RiskManager &risk_mgr,
               const FlowImbalanceGuard *flow_guard)
    : config_{config}, fv_engine_{std::move(fv_engine)}, order_mgr_{order_mgr},
      risk_mgr_{risk_mgr}, flow_guard_{flow_guard} {}

std::pair<int, int> Quoter::compute_quotes(double fv_cents, int half_spread,
                                           double inventory_skew_cents) {
  const auto bid_val = static_cast<int>(std::round(
      fv_cents - static_cast<double>(half_spread) - inventory_skew_cents));
  const auto ask_val = static_cast<int>(std::round(
      fv_cents + static_cast<double>(half_spread) - inventory_skew_cents));
  return {std::clamp(bid_val, kBidMinCents, kBidMaxCents),
          std::clamp(ask_val, kAskMinCents, kAskMaxCents)};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<int> Quoter::passive_bid(int desired_bid, int market_ask_cents) {
  const int max_passive = market_ask_cents - 1; // ≥1c behind the market ask
  if (max_passive < kBidMinCents) {
    return std::nullopt; // ask at the floor — no room to bid passively
  }
  return std::min(desired_bid, max_passive);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<int> Quoter::passive_ask(int desired_ask, int market_bid_cents) {
  const int min_passive = market_bid_cents + 1; // ≥1c above the market bid
  if (min_passive > kAskMaxCents) {
    return std::nullopt; // bid at the ceiling — no room to ask passively
  }
  return std::max(desired_ask, min_passive);
}

void Quoter::refresh_bid(const std::string &ticker, int desired_bid) {
  auto &live = live_quotes_[ticker];
  // A filled/vanished order is erased from open_orders: cancelling its dead id
  // fails forever. Drop it and re-place instead.
  if (!live.bid_order_id.empty() &&
      !order_mgr_.open_orders().contains(live.bid_order_id)) {
    live.bid_order_id.clear();
  }
  if (live.bid_order_id.empty()) {
    // Self-cross guard: don't place bid if it would match our own resting ask.
    if (!live.ask_order_id.empty() && desired_bid >= live.current_ask_cents) {
      return;
    }
    if (!risk_mgr_.check_order(ticker, Side::Yes, desired_bid,
                               config_.quote_size)) {
      return;
    }
    Order order =
        order_mgr_.place(ticker, Side::Yes, desired_bid, config_.quote_size);
    live.bid_order_id = order.id;
    live.current_bid_cents = desired_bid;
  } else if (std::abs(live.current_bid_cents - desired_bid) >
             config_.reprice_threshold_cents) {
    if (!order_mgr_.cancel(live.bid_order_id)) {
      return;
    }
    live.bid_order_id.clear();
    // Self-cross guard: after cancelling, don't re-enter if new bid ≥ ask.
    if (!live.ask_order_id.empty() && desired_bid >= live.current_ask_cents) {
      return;
    }
    if (!risk_mgr_.check_order(ticker, Side::Yes, desired_bid,
                               config_.quote_size)) {
      return;
    }
    Order order =
        order_mgr_.place(ticker, Side::Yes, desired_bid, config_.quote_size);
    live.bid_order_id = order.id;
    live.current_bid_cents = desired_bid;
  }
}

void Quoter::refresh_ask(const std::string &ticker, int desired_ask) {
  const int no_price = complement_price(desired_ask);
  auto &live = live_quotes_[ticker];
  if (!live.ask_order_id.empty() &&
      !order_mgr_.open_orders().contains(live.ask_order_id)) {
    live.ask_order_id.clear();
  }
  if (live.ask_order_id.empty()) {
    // Self-cross guard: don't place ask if it would match our own resting bid.
    if (!live.bid_order_id.empty() && desired_ask <= live.current_bid_cents) {
      return;
    }
    if (!risk_mgr_.check_order(ticker, Side::No, no_price,
                               config_.quote_size)) {
      return;
    }
    Order order =
        order_mgr_.place(ticker, Side::No, no_price, config_.quote_size);
    live.ask_order_id = order.id;
    live.current_ask_cents = desired_ask;
  } else if (std::abs(live.current_ask_cents - desired_ask) >
             config_.reprice_threshold_cents) {
    if (!order_mgr_.cancel(live.ask_order_id)) {
      return;
    }
    live.ask_order_id.clear();
    // Self-cross guard: after cancelling, don't re-enter if new ask ≤ bid.
    if (!live.bid_order_id.empty() && desired_ask <= live.current_bid_cents) {
      return;
    }
    if (!risk_mgr_.check_order(ticker, Side::No, no_price,
                               config_.quote_size)) {
      return;
    }
    Order order =
        order_mgr_.place(ticker, Side::No, no_price, config_.quote_size);
    live.ask_order_id = order.id;
    live.current_ask_cents = desired_ask;
  }
}

void Quoter::update(std::string_view ticker, const LocalOrderbook &book) {
  const std::optional<Level> best_bid = book.best_bid();
  const std::optional<Level> best_ask = book.best_ask();
  if (!best_bid.has_value() || !best_ask.has_value()) {
    return;
  }

  const std::string ticker_str{ticker};
  const double mid = book.mid_price_cents();
  const double fair_val =
      fv_engine_.estimate(FairValueInput{mid, kDefaultTimeToCloseHours, 0, {}});
  // A non-finite fair value (NaN/inf from broken model math) would be cast to
  // int in compute_quotes — undefined behavior that yields a garbage price.
  // This must be impossible; flatten and crash rather than quote on it.
  ensure(std::isfinite(fair_val), "fair value is not finite");

  int target_spread = config_.target_spread_cents;
  if (flow_guard_ != nullptr && flow_guard_->is_imbalanced(ticker_str)) {
    target_spread += config_.imbalance_spread_cents;
    get_logger()->debug(
        "flow imbalanced ticker={} ratio={:.2f} — widening spread to {}c",
        ticker, flow_guard_->imbalance_ratio(ticker_str), target_spread);
  }
  // Apply the spread floor: never quote tighter than min_spread_cents.
  const int base_half_spread = std::max(
      {kHalfSpreadMin, target_spread / 2, config_.min_spread_cents / 2});
  // Maker fee: widen so the net-of-fee edge is preserved. Kalshi's per-contract
  // fee is γ·P·(1−P); estimate P from the fair value.
  int fee_cents = 0;
  if (config_.maker_fee_rate > 0.0) {
    const double prob = fair_val / kContractMaxCents;
    fee_cents = static_cast<int>(std::round(config_.maker_fee_rate * prob *
                                            (1.0 - prob) * kContractMaxCents));
  }
  const int half_spread = base_half_spread + fee_cents;
  const double inventory_skew =
      static_cast<double>(order_mgr_.net_position(ticker_str)) *
      config_.skew_per_contract_cents;

  const auto [raw_bid, raw_ask] =
      compute_quotes(fair_val, half_spread, inventory_skew);

  // Stay strictly passive vs. the observed BBO: never submit a price at or
  // through the touch.
  const int market_bid = best_bid->price_cents;
  const int market_ask = best_ask->price_cents;
  const std::optional<int> desired_bid = passive_bid(raw_bid, market_ask);
  const std::optional<int> desired_ask = passive_ask(raw_ask, market_bid);

  get_logger()->debug(
      "reprice ticker={} mid={:.1f} fv={:.2f} raw_bid={} raw_ask={} bid={} "
      "ask={}",
      ticker, mid, fair_val, raw_bid, raw_ask, desired_bid.value_or(-1),
      desired_ask.value_or(-1));

  if (desired_bid.has_value()) {
    refresh_bid(ticker_str, *desired_bid);
  }
  if (desired_ask.has_value()) {
    refresh_ask(ticker_str, *desired_ask);
  }
}

void Quoter::reset_quotes() { live_quotes_.clear(); }

} // namespace kalshi
