#pragma once

#include "auth.hpp"
#include "clock_skew.hpp"
#include "http_transport.hpp"
#include "rate_limiter.hpp"
#include "types.hpp"

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
  std::vector<Order> get_open_orders();

  // Fetches all market positions from the exchange (paginated). This is the
  // authoritative position state used to reconcile against local accounting.
  std::vector<MarketPosition> get_positions();

  // Fetches active liquidity incentive pools (public endpoint, paginated).
  // Used to bias scanner ranking toward markets that pay for resting size.
  std::vector<IncentiveProgram> get_incentive_programs();

  // Fetches fills from the exchange (paginated), oldest cutoff controlled by
  // min_ts_seconds (0 = no cutoff). Used to backfill fills that arrived while
  // the WS fill channel was disconnected; replaying them through record_fill
  // is duplicate-safe (dedup by trade_id).
  std::vector<Fill> get_fills(long long min_ts_seconds = 0);

  // Measures local-vs-server clock skew from the Date header of a GET
  // /exchange/status response. Works even when the request itself is rejected
  // (a skewed clock 401s every signed request — the very condition being
  // detected). Empty when the response carries no parseable Date header.
  std::optional<std::chrono::seconds> measure_clock_skew(
      SystemTimePoint local_now = std::chrono::system_clock::now());

private:
  Auth auth_;
  std::unique_ptr<IHttpTransport> transport_;
  std::string base_url_;
  std::string
      path_prefix_; // URL path component of base_url (e.g. /trade-api/v2)
  RateLimiter write_limiter_;
};

} // namespace kalshi
