#pragma once

#include "core/rate_limiter.hpp"
#include "core/types.hpp"
#include "net/auth.hpp"
#include "net/http_transport.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kalshi {

class RestClient {
public:
  RestClient(
      Auth auth, std::unique_ptr<IHttpTransport> transport,
      std::string base_url = "https://trading-api.kalshi.com/trade-api/v2");

  std::vector<Market> get_markets(std::string_view event_ticker = "");
  Orderbook get_orderbook(std::string_view ticker);
  Order place_order(std::string_view ticker, Side side, int price_cents,
                    int quantity, OrderType type);
  Order place_order(std::string_view ticker, Side side, int price_cents,
                    Quantity count, OrderType type);

  // Closes a net position (positive = long YES, negative = long NO) with an
  // aggressive immediate-or-cancel taker order. No-op when already flat.
  Order flatten(std::string_view ticker, Quantity net_position);

  bool cancel_order(std::string_view order_id);

  // Amend a resting order in place (V2): one atomic call replaces the
  // cancel+replace pair — no quote-less window. Priority is assumed lost on
  // a price change (pinned by the demo conformance suite).
  // Returns the resulting order id on success — the official docs show the
  // response may carry a NEW order_id (the demo returns the same one); track
  // whatever comes back.
  std::optional<std::string> amend_order(std::string_view order_id,
                                         std::string_view ticker, Side side,
                                         int new_price_cents, Quantity count);

  // Shrink a resting order without touching its queue priority (V2).
  // Returns the remaining count on success.
  std::optional<Quantity> decrease_order(std::string_view order_id,
                                         Quantity reduce_to);
  std::vector<Order> get_open_orders();

  // Fetches all market positions from the exchange (paginated). This is the
  // authoritative position state used to reconcile against local accounting.
  std::vector<MarketPosition> get_positions();

  // Fetches active liquidity incentive pools (public endpoint, paginated).
  // Used to bias scanner ranking toward markets that pay for resting size.
  std::vector<IncentiveProgram> get_incentive_programs();

  // Most recent public trades for a market, newest first (GET
  // /markets/trades?limit=N). An empty vector means the market has definitively
  // never traded; nullopt means the probe failed (callers fail open). Feeds
  // the scanner's liveness, flow-rate, and tape-range admission (items
  // 49/61/62): vol_24h says yesterday, this says now.
  std::optional<std::vector<PublicTrade>>
  get_recent_trades(std::string_view ticker, int limit);

  // Trailing 1-minute candlesticks for a market (GET /series/{s}/markets/
  // {t}/candlesticks). Only the trade-price close per period is kept — the
  // input for the reversion-score gate (item 67): K = sum |dclose| vs z =
  // net change. nullopt = probe failed (callers fail open); a period with
  // no trades has close_cents absent.
  std::optional<std::vector<Candle>>
  get_candlesticks(std::string_view ticker, long long start_ts_seconds,
                   long long end_ts_seconds);

  // Fetches fills from the exchange (paginated), oldest cutoff controlled by
  // min_ts_seconds (0 = no cutoff). Used to backfill fills that arrived while
  // the WS fill channel was disconnected; replaying them through record_fill
  // is duplicate-safe (dedup by trade_id).
  std::vector<Fill> get_fills(long long min_ts_seconds = 0);

private:
  Auth auth_;
  std::unique_ptr<IHttpTransport> transport_;
  std::string base_url_;
  std::string
      path_prefix_; // URL path component of base_url (e.g. /trade-api/v2)
  RateLimiter write_limiter_;
};

} // namespace kalshi
