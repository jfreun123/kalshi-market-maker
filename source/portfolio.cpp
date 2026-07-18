#include "portfolio.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace kalshi {

std::string event_ticker_of(std::string_view market_ticker) {
  const auto last_hyphen = market_ticker.rfind('-');
  if (last_hyphen == std::string_view::npos) {
    return std::string{market_ticker};
  }
  return std::string{market_ticker.substr(0, last_hyphen)};
}

Reconciliation
reconcile(const IOrderManager &order_mgr,
          const std::vector<std::string> &tickers,
          const std::vector<MarketPosition> &exchange_positions,
          const std::vector<MarketPosition> &baseline_positions) {
  std::unordered_map<std::string, Quantity> exchange_by_ticker;
  for (const auto &position : exchange_positions) {
    exchange_by_ticker[position.ticker] = position.position;
  }

  const std::unordered_set<std::string> traded{tickers.begin(), tickers.end()};
  std::unordered_map<std::string, Quantity> baseline_by_ticker;
  for (const auto &position : baseline_positions) {
    if (!position.position.is_zero() && !traded.contains(position.ticker)) {
      baseline_by_ticker[position.ticker] = position.position;
    }
  }

  // Check the tracked universe plus any ticker the exchange or baseline
  // reports a non-zero position in (so positions we don't track still
  // surface).
  std::unordered_set<std::string> to_check{tickers.begin(), tickers.end()};
  for (const auto &position : exchange_positions) {
    if (!position.position.is_zero()) {
      to_check.insert(position.ticker);
    }
  }
  for (const auto &[ticker, position] : baseline_by_ticker) {
    to_check.insert(ticker);
  }

  Reconciliation result;
  for (const auto &ticker : to_check) {
    const Quantity local = order_mgr.net_position(ticker);
    auto exch_it = exchange_by_ticker.find(ticker);
    const Quantity exchange =
        exch_it == exchange_by_ticker.end() ? Quantity{} : exch_it->second;
    auto base_it = baseline_by_ticker.find(ticker);
    const Quantity baseline =
        base_it == baseline_by_ticker.end() ? Quantity{} : base_it->second;
    if (local + baseline != exchange) {
      result.diffs.push_back({ticker, local, exchange, baseline});
    }
  }

  std::sort(result.diffs.begin(), result.diffs.end(),
            [](const PositionDiff &lhs, const PositionDiff &rhs) {
              return lhs.ticker < rhs.ticker;
            });
  result.in_sync = result.diffs.empty();
  return result;
}

Portfolio::Portfolio(const IOrderManager &order_mgr) : order_mgr_{order_mgr} {}

PortfolioSnapshot Portfolio::snapshot(const std::vector<std::string> &tickers,
                                      const MarkMap &marks) const {
  PortfolioSnapshot snap;

  // Accumulate per-event so correlated strikes roll up together.
  std::unordered_map<std::string, EventExposure> events;

  for (const auto &ticker : tickers) {
    const double realized = order_mgr_.realized_pnl(ticker);
    const double notional = order_mgr_.position_cost(ticker);

    double unrealized = 0.0;
    auto mark_it = marks.find(ticker);
    if (mark_it != marks.end()) {
      unrealized = order_mgr_.unrealized_pnl(ticker, mark_it->second);
    }

    snap.total_realized_cents += realized;
    snap.total_unrealized_cents += unrealized;
    snap.total_notional_cents += notional;

    EventExposure &event = events[event_ticker_of(ticker)];
    event.realized_pnl_cents += realized;
    event.unrealized_pnl_cents += unrealized;
    event.notional_cost_cents += notional;
    ++event.market_count;
  }

  snap.by_event.reserve(events.size());
  for (auto &[event_ticker, exposure] : events) {
    exposure.event_ticker = event_ticker;
    snap.by_event.push_back(std::move(exposure));
  }
  std::sort(snap.by_event.begin(), snap.by_event.end(),
            [](const EventExposure &lhs, const EventExposure &rhs) {
              return lhs.notional_cost_cents > rhs.notional_cost_cents;
            });

  return snap;
}

} // namespace kalshi
