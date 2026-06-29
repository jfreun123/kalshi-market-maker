#include "portfolio.hpp"

#include <algorithm>
#include <string>

namespace kalshi {

std::string event_ticker_of(std::string_view market_ticker) {
  const auto last_hyphen = market_ticker.rfind('-');
  if (last_hyphen == std::string_view::npos) {
    return std::string{market_ticker};
  }
  return std::string{market_ticker.substr(0, last_hyphen)};
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
