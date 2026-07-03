#include "quoter.hpp"

#include "ensure.hpp"
#include "flow_imbalance.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
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

namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<int> passive_bid(int desired_bid, int market_ask_cents) {
  const int max_passive = market_ask_cents - 1;
  if (max_passive < kBidMinCents) {
    return std::nullopt;
  }
  return std::min(desired_bid, max_passive);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<int> passive_ask(int desired_ask, int market_bid_cents) {
  const int min_passive = market_bid_cents + 1;
  if (min_passive > kAskMaxCents) {
    return std::nullopt;
  }
  return std::max(desired_ask, min_passive);
}

} // namespace

void Quoter::refresh_bid(const std::string &ticker, OwnQuote &own,
                         int desired_bid) {
  if (own.bid_order_id.empty()) {
    // Self-cross guard: don't place bid if it would match our own resting ask.
    if (!own.ask_order_id.empty() && desired_bid >= own.quoted_ask_cents) {
      return;
    }
    if (!risk_mgr_.check_order(ticker, Side::Yes, desired_bid,
                               config_.quote_size)) {
      return;
    }
    Order order =
        order_mgr_.place(ticker, Side::Yes, desired_bid, config_.quote_size);
    own.bid_order_id = order.id;
    own.quoted_bid_cents = desired_bid;
  } else if (std::abs(own.quoted_bid_cents - desired_bid) >
             config_.reprice_threshold_cents) {
    if (!release_order(own.bid_order_id)) {
      return;
    }
    own.bid_order_id.clear();
    // Self-cross guard: after cancelling, don't re-enter if new bid ≥ ask.
    if (!own.ask_order_id.empty() && desired_bid >= own.quoted_ask_cents) {
      return;
    }
    if (!risk_mgr_.check_order(ticker, Side::Yes, desired_bid,
                               config_.quote_size)) {
      return;
    }
    Order order =
        order_mgr_.place(ticker, Side::Yes, desired_bid, config_.quote_size);
    own.bid_order_id = order.id;
    own.quoted_bid_cents = desired_bid;
  }
}

void Quoter::refresh_ask(const std::string &ticker, OwnQuote &own,
                         int desired_ask) {
  const int no_price = complement_price(desired_ask);
  if (own.ask_order_id.empty()) {
    // Self-cross guard: don't place ask if it would match our own resting bid.
    if (!own.bid_order_id.empty() && desired_ask <= own.quoted_bid_cents) {
      return;
    }
    if (!risk_mgr_.check_order(ticker, Side::No, no_price,
                               config_.quote_size)) {
      return;
    }
    Order order =
        order_mgr_.place(ticker, Side::No, no_price, config_.quote_size);
    own.ask_order_id = order.id;
    own.quoted_ask_cents = desired_ask;
  } else if (std::abs(own.quoted_ask_cents - desired_ask) >
             config_.reprice_threshold_cents) {
    if (!release_order(own.ask_order_id)) {
      return;
    }
    own.ask_order_id.clear();
    // Self-cross guard: after cancelling, don't re-enter if new ask ≤ bid.
    if (!own.bid_order_id.empty() && desired_ask <= own.quoted_bid_cents) {
      return;
    }
    if (!risk_mgr_.check_order(ticker, Side::No, no_price,
                               config_.quote_size)) {
      return;
    }
    Order order =
        order_mgr_.place(ticker, Side::No, no_price, config_.quote_size);
    own.ask_order_id = order.id;
    own.quoted_ask_cents = desired_ask;
  }
}

void Quoter::update(std::string_view ticker, const LocalOrderbook &book) {
  const std::string ticker_str{ticker};
  OwnQuote &own = own_quotes_[ticker_str];
  // Price off the book minus our own resting quotes: the exchange book echoes
  // them back, and on a thin book our own size dominates the micro-price —
  // feeding it back produces a self-referential cancel/replace oscillation
  // (demo finding D4).
  const LocalOrderbook &visible = book_without_own_quotes(own, book);
  const std::optional<Level> best_bid = visible.best_bid();
  const std::optional<Level> best_ask = visible.best_ask();
  if (!best_bid.has_value() || !best_ask.has_value()) {
    return;
  }

  const double mid = visible.mid_price_cents();
  // Anchor fair value on the volume-adjusted micro-price, not the raw mid, so
  // quotes lean toward the side the book is pressuring (less adverse
  // selection).
  const double micro = visible.micro_price_cents();
  const double fair_val = fv_engine_.estimate(
      FairValueInput{micro, kDefaultTimeToCloseHours, 0, {}});
  ensure(std::isfinite(fair_val), "fair value is not finite");

  int target_spread = config_.target_spread_cents;
  if (flow_guard_ != nullptr && flow_guard_->is_imbalanced(ticker_str)) {
    target_spread += config_.imbalance_spread_cents;
    get_logger()->debug(
        "flow imbalanced ticker={} ratio={:.2f} — widening spread to {}c",
        ticker, flow_guard_->imbalance_ratio(ticker_str), target_spread);
  }
  // Apply the spread floor: never quote tighter than min_spread_cents. The
  // floor's half-spread rounds up so an odd floor still holds.
  const int base_half_spread = std::max(
      {kHalfSpreadMin, target_spread / 2, (config_.min_spread_cents + 1) / 2});
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
      order_mgr_.net_position(ticker_str).contracts() *
      config_.skew_per_contract_cents;

  const auto [raw_bid, raw_ask] =
      compute_quotes(fair_val, half_spread, inventory_skew);

  const int market_bid = best_bid->price_cents;
  const int market_ask = best_ask->price_cents;
  const std::optional<int> desired_bid = passive_bid(raw_bid, market_ask);
  const std::optional<int> desired_ask = passive_ask(raw_ask, market_bid);

  get_logger()->debug(
      "reprice ticker={} mid={:.1f} micro={:.2f} fv={:.2f} raw_bid={} "
      "raw_ask={} bid={} ask={}",
      ticker, mid, micro, fair_val, raw_bid, raw_ask, desired_bid.value_or(-1),
      desired_ask.value_or(-1));

  if (desired_bid.has_value()) {
    refresh_bid(ticker_str, own, *desired_bid);
  }
  if (desired_ask.has_value()) {
    refresh_ask(ticker_str, own, *desired_ask);
  }
}

void Quoter::reset_quotes() { own_quotes_.clear(); }

const LocalOrderbook &
Quoter::book_without_own_quotes(const OwnQuote &own,
                                const LocalOrderbook &book) {
  if (own.bid_order_id.empty() && own.ask_order_id.empty()) {
    return book;
  }
  scratch_book_ = book;
  const auto &open_orders = order_mgr_.open_orders();
  if (!own.bid_order_id.empty()) {
    const auto order_it = open_orders.find(own.bid_order_id);
    if (order_it != open_orders.end()) {
      scratch_book_.apply_delta(Side::Yes, own.quoted_bid_cents,
                                -order_it->second.remaining_quantity());
    }
  }
  if (!own.ask_order_id.empty()) {
    const auto order_it = open_orders.find(own.ask_order_id);
    if (order_it != open_orders.end()) {
      scratch_book_.apply_delta(Side::No,
                                complement_price(own.quoted_ask_cents),
                                -order_it->second.remaining_quantity());
    }
  }
  return scratch_book_;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - ticker vs order id
void Quoter::forget_order(std::string_view ticker, std::string_view order_id) {
  auto own_it = own_quotes_.find(std::string{ticker});
  if (own_it == own_quotes_.end()) {
    return;
  }
  if (own_it->second.bid_order_id == order_id) {
    own_it->second.bid_order_id.clear();
  }
  if (own_it->second.ask_order_id == order_id) {
    own_it->second.ask_order_id.clear();
  }
}

bool Quoter::release_order(const std::string &order_id) {
  if (order_mgr_.cancel(order_id)) {
    return true;
  }
  // The exchange rejected the cancel. If the order manager no longer tracks
  // the order it was already filled or cancelled out-of-band — safe to forget
  // and re-quote. If it is still tracked the rejection is transient (e.g. a
  // 429); keep the quote state untouched and retry on a later update.
  if (!order_mgr_.open_orders().contains(order_id)) {
    get_logger()->warn(
        "cancel rejected for order_id={} which is no longer tracked — "
        "forgetting stale quote",
        order_id);
    return true;
  }
  return false;
}

} // namespace kalshi
