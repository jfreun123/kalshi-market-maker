#include "quoter.hpp"

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

Quoter::Quoter(QuoterConfig config, OrderManager &order_mgr,
               RiskManager &risk_mgr)
    : Quoter(config, FairValueEngine{std::make_unique<HeuristicModel>()},
             order_mgr, risk_mgr) {}

Quoter::Quoter(QuoterConfig config, FairValueEngine fv_engine,
               OrderManager &order_mgr, RiskManager &risk_mgr)
    : config_{config}, fv_engine_{std::move(fv_engine)}, order_mgr_{order_mgr},
      risk_mgr_{risk_mgr} {}

std::pair<int, int> Quoter::compute_quotes(double fv_cents, int half_spread,
                                           double inventory_skew_cents) {
  const auto bid_val = static_cast<int>(std::round(
      fv_cents - static_cast<double>(half_spread) - inventory_skew_cents));
  const auto ask_val = static_cast<int>(std::round(
      fv_cents + static_cast<double>(half_spread) - inventory_skew_cents));
  return {std::clamp(bid_val, kBidMinCents, kBidMaxCents),
          std::clamp(ask_val, kAskMinCents, kAskMaxCents)};
}

void Quoter::refresh_bid(const std::string &ticker, int desired_bid) {
  auto &live = live_quotes_[ticker];
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
  if (!book.best_bid().has_value() || !book.best_ask().has_value()) {
    return;
  }

  const std::string ticker_str{ticker};
  const double mid = book.mid_price_cents();
  const double fair_val =
      fv_engine_.estimate(FairValueInput{mid, kDefaultTimeToCloseHours, 0, {}});

  const int half_spread =
      std::max(kHalfSpreadMin, config_.target_spread_cents / 2);
  const double inventory_skew =
      static_cast<double>(order_mgr_.net_position(ticker_str)) *
      config_.skew_per_contract_cents;

  const auto [desired_bid, desired_ask] =
      compute_quotes(fair_val, half_spread, inventory_skew);

  get_logger()->debug("reprice ticker={} mid={:.1f} fv={:.2f} bid={} ask={}",
                      ticker, mid, fair_val, desired_bid, desired_ask);

  refresh_bid(ticker_str, desired_bid);
  refresh_ask(ticker_str, desired_ask);
}

} // namespace kalshi
