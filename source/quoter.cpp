#include "quoter.hpp"

#include "analytics.hpp"
#include "ensure.hpp"
#include "flow_imbalance.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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
// Skew-per-fill cap: one quote-sized fill must shift the reservation by less
// than the half-spread, or every calm-market round trip locks in a loss
// (buy 50 -> offer 49). Near 50c the price moves ~25c per log-odds unit, so
// b >= 25 x quote_size holds a single-fill shift to ~1c.
constexpr double kMinBPerQuoteLot = 25.0;

Quoter::Quoter(QuoterConfig config, IOrderManager &order_mgr,
               RiskManager &risk_mgr, const FlowImbalanceGuard *flow_guard,
               Clock clock)
    : Quoter(config, FairValueEngine{std::make_unique<HeuristicModel>()},
             order_mgr, risk_mgr, flow_guard, std::move(clock)) {}

Quoter::Quoter(QuoterConfig config, FairValueEngine fv_engine,
               IOrderManager &order_mgr, RiskManager &risk_mgr,
               const FlowImbalanceGuard *flow_guard, Clock clock)
    : config_{config}, fv_engine_{std::move(fv_engine)}, order_mgr_{order_mgr},
      risk_mgr_{risk_mgr}, flow_guard_{flow_guard},
      clock_{clock ? std::move(clock)
                   : Clock{[] { return std::chrono::steady_clock::now(); }}},
      inventory_b_contracts_{
          std::max(lmsr_b_from_risk(risk_mgr.limits().max_position_per_market,
                                    risk_mgr.limits().max_quote_price_cents),
                   kMinBPerQuoteLot * config.quote_size)} {}

double lmsr_b_from_risk(int max_position_contracts, int max_quote_price_cents) {
  const double upper = static_cast<double>(max_quote_price_cents);
  constexpr double kHalfContract = kContractMaxCents / 2.0;
  if (max_position_contracts <= 0 || upper <= kHalfContract ||
      upper >= kContractMaxCents) {
    return std::numeric_limits<double>::infinity();
  }
  return static_cast<double>(max_position_contracts) /
         std::log(upper / (kContractMaxCents - upper));
}

double lmsr_skewed_fair_value(double fv_cents, double net_inventory_contracts,
                              double b_contracts) {
  if (!std::isfinite(b_contracts) || net_inventory_contracts == 0.0) {
    return fv_cents;
  }
  const double opposing_odds = kContractMaxCents / fv_cents - 1.0;
  const double shifted_odds =
      opposing_odds * std::exp(net_inventory_contracts / b_contracts);
  return kContractMaxCents / (1.0 + shifted_odds);
}

bool Quoter::resting_too_young(std::chrono::steady_clock::time_point placed_at,
                               std::chrono::steady_clock::time_point now,
                               int rest_ms) {
  return now - placed_at < std::chrono::milliseconds{rest_ms};
}

std::pair<int, int> Quoter::compute_quotes(double fv_cents, int half_spread) {
  const auto bid_val =
      static_cast<int>(std::floor(fv_cents - static_cast<double>(half_spread)));
  const auto ask_val =
      static_cast<int>(std::ceil(fv_cents + static_cast<double>(half_spread)));
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
                         int desired_bid,
                         std::chrono::steady_clock::time_point now) {
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
    own.bid_placed_at = now;
  } else if (std::abs(own.quoted_bid_cents - desired_bid) >
             config_.reprice_threshold_cents) {
    const bool adverse_jump =
        own.quoted_bid_cents - desired_bid >= config_.theo_jump_cents;
    if (adverse_jump && !own.bid_fade_pending) {
      own.bid_fade_pending = true;
      get_logger()->debug(
          "fade pending ticker={} side=bid quoted={} desired={} — awaiting "
          "confirmation on the next update",
          ticker, own.quoted_bid_cents, desired_bid);
      return;
    }
    if (!adverse_jump) {
      own.bid_fade_pending = false;
    }
    const int rest_ms =
        adverse_jump ? config_.fade_rest_ms : config_.min_rest_ms;
    if (resting_too_young(own.bid_placed_at, now, rest_ms)) {
      get_logger()->debug(
          "reprice suppressed ticker={} side=bid quoted={} desired={} — "
          "resting under {}ms",
          ticker, own.quoted_bid_cents, desired_bid, rest_ms);
      return;
    }
    if (adverse_jump) {
      own.bid_fade_pending = false;
      get_logger()->info(
          "theo fade ticker={} side=bid quoted={} desired={} — fair value "
          "jumped against the resting bid",
          ticker, own.quoted_bid_cents, desired_bid);
    }
    // Self-cross guard first: an amend that crosses our own ask must fall
    // back to a plain cancel (no re-entry).
    const bool would_cross =
        !own.ask_order_id.empty() && desired_bid >= own.quoted_ask_cents;
    if (!would_cross && risk_mgr_.check_order(ticker, Side::Yes, desired_bid,
                                              config_.quote_size)) {
      if (const auto amended_id =
              order_mgr_.amend(own.bid_order_id, ticker, Side::Yes, desired_bid,
                               Quantity::from_contracts(config_.quote_size))) {
        own.bid_order_id = *amended_id;
        own.quoted_bid_cents = desired_bid;
        own.bid_placed_at = now;
        return;
      }
    }
    if (!release_order(own.bid_order_id)) {
      return;
    }
    own.bid_order_id.clear();
    // Self-cross guard: after cancelling, don't re-enter if new bid ≥ ask.
    if (would_cross) {
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
    own.bid_placed_at = now;
  }
}

void Quoter::refresh_ask(const std::string &ticker, OwnQuote &own,
                         int desired_ask,
                         std::chrono::steady_clock::time_point now) {
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
    own.ask_placed_at = now;
  } else if (std::abs(own.quoted_ask_cents - desired_ask) >
             config_.reprice_threshold_cents) {
    const bool adverse_jump =
        desired_ask - own.quoted_ask_cents >= config_.theo_jump_cents;
    if (adverse_jump && !own.ask_fade_pending) {
      own.ask_fade_pending = true;
      get_logger()->debug(
          "fade pending ticker={} side=ask quoted={} desired={} — awaiting "
          "confirmation on the next update",
          ticker, own.quoted_ask_cents, desired_ask);
      return;
    }
    if (!adverse_jump) {
      own.ask_fade_pending = false;
    }
    const int rest_ms =
        adverse_jump ? config_.fade_rest_ms : config_.min_rest_ms;
    if (resting_too_young(own.ask_placed_at, now, rest_ms)) {
      get_logger()->debug(
          "reprice suppressed ticker={} side=ask quoted={} desired={} — "
          "resting under {}ms",
          ticker, own.quoted_ask_cents, desired_ask, rest_ms);
      return;
    }
    if (adverse_jump) {
      own.ask_fade_pending = false;
      get_logger()->info(
          "theo fade ticker={} side=ask quoted={} desired={} — fair value "
          "jumped against the resting ask",
          ticker, own.quoted_ask_cents, desired_ask);
    }
    const bool would_cross =
        !own.bid_order_id.empty() && desired_ask <= own.quoted_bid_cents;
    if (!would_cross &&
        risk_mgr_.check_order(ticker, Side::No, no_price, config_.quote_size)) {
      if (const auto amended_id =
              order_mgr_.amend(own.ask_order_id, ticker, Side::No, no_price,
                               Quantity::from_contracts(config_.quote_size))) {
        own.ask_order_id = *amended_id;
        own.quoted_ask_cents = desired_ask;
        own.ask_placed_at = now;
        return;
      }
    }
    if (!release_order(own.ask_order_id)) {
      return;
    }
    own.ask_order_id.clear();
    // Self-cross guard: after cancelling, don't re-enter if new ask ≤ bid.
    if (would_cross) {
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
    own.ask_placed_at = now;
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
  if (best_bid->price_cents >= best_ask->price_cents) {
    get_logger()->warn(
        "crossed visible book ticker={} bid={} ask={} — keeping resting quotes",
        ticker, best_bid->price_cents, best_ask->price_cents);
    return;
  }

  const double mid = visible.mid_price_cents();
  // Anchor fair value on the volume-adjusted micro-price, not the raw mid, so
  // quotes lean toward the side the book is pressuring (less adverse
  // selection).
  const double micro = visible.micro_price_cents();
  const auto ema_it = fv_ema_.find(ticker_str);
  const double smoothed_micro =
      (ema_it == fv_ema_.end())
          ? micro
          : config_.fv_ema_alpha * micro +
                (1.0 - config_.fv_ema_alpha) * ema_it->second;
  fv_ema_[ticker_str] = smoothed_micro;
  const double fair_val = fv_engine_.estimate(
      FairValueInput{smoothed_micro, kDefaultTimeToCloseHours, 0, {}});
  ensure(std::isfinite(fair_val), "fair value is not finite");

  int target_spread = config_.target_spread_cents;
  const bool imbalanced =
      flow_guard_ != nullptr && flow_guard_->is_imbalanced(ticker_str);
  if (imbalanced) {
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
  const double inventory = order_mgr_.net_position(ticker_str).contracts();
  double leaned_val = fair_val;
  if (imbalanced && flow_guard_ != nullptr) {
    const auto taker_side = flow_guard_->dominant_taker_side(ticker_str);
    if (taker_side.has_value()) {
      leaned_val += (*taker_side == Side::Yes) ? config_.flow_lean_cents
                                               : -config_.flow_lean_cents;
    }
  }
  const double reservation_val =
      lmsr_skewed_fair_value(leaned_val, inventory, inventory_b_contracts_);

  auto [raw_bid, raw_ask] = compute_quotes(reservation_val, half_spread);
  if (raw_bid < config_.longshot_price_threshold_cents) {
    raw_bid = std::max(kBidMinCents, raw_bid - config_.longshot_edge_cents);
  }
  if (complement_price(raw_ask) < config_.longshot_price_threshold_cents) {
    raw_ask = std::min(kAskMaxCents, raw_ask + config_.longshot_edge_cents);
  }
  if (inventory > 0.0) {
    raw_ask = std::max(
        raw_bid + 1,
        static_cast<int>(
            std::ceil(reservation_val + config_.unwind_edge_cents)));
  } else if (inventory < 0.0) {
    raw_bid = std::min(
        raw_ask - 1,
        static_cast<int>(
            std::floor(reservation_val - config_.unwind_edge_cents)));
  }

  const int market_bid = best_bid->price_cents;
  const int market_ask = best_ask->price_cents;
  const std::optional<int> desired_bid = passive_bid(raw_bid, market_ask);
  const std::optional<int> desired_ask = passive_ask(raw_ask, market_bid);

  get_logger()->debug(
      "reprice ticker={} mid={:.1f} micro={:.2f} fv={:.2f} resv={:.2f} "
      "raw_bid={} raw_ask={} bid={} ask={}",
      ticker, mid, micro, fair_val, reservation_val, raw_bid, raw_ask,
      desired_bid.value_or(-1), desired_ask.value_or(-1));

  if (analytics_ != nullptr) {
    analytics_->quote_decision(
        {ticker, mid, micro, fair_val, desired_bid.value_or(-1),
         desired_ask.value_or(-1), inventory, imbalanced});
  }

  const std::chrono::steady_clock::time_point now = clock_();
  const double inventory_cap =
      static_cast<double>(config_.inventory_cap_lots) * config_.quote_size;
  const bool suppress_bid =
      (reduce_only_ && inventory >= 0.0) || inventory >= inventory_cap;
  const bool suppress_ask =
      (reduce_only_ && inventory <= 0.0) || inventory <= -inventory_cap;
  if (suppress_bid && !own.bid_order_id.empty() &&
      release_order(own.bid_order_id)) {
    own.bid_order_id.clear();
  }
  if (suppress_ask && !own.ask_order_id.empty() &&
      release_order(own.ask_order_id)) {
    own.ask_order_id.clear();
  }
  if (desired_bid.has_value() && !suppress_bid) {
    refresh_bid(ticker_str, own, *desired_bid, now);
  }
  if (desired_ask.has_value() && !suppress_ask) {
    refresh_ask(ticker_str, own, *desired_ask, now);
  }
}

void Quoter::set_reduce_only(bool reduce_only) { reduce_only_ = reduce_only; }

void Quoter::set_analytics(AnalyticsLogger *analytics) {
  analytics_ = analytics;
}

void Quoter::reset_quotes() {
  own_quotes_.clear();
  fv_ema_.clear();
}

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
