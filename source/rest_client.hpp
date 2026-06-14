#pragma once

#include "auth.hpp"
#include "http_transport.hpp"
#include "types.hpp"

#include <memory>
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
  bool cancel_order(std::string_view order_id);
  std::vector<Order> get_open_orders();

private:
  Auth auth_;
  std::unique_ptr<IHttpTransport> transport_;
  std::string base_url_;
  std::string
      path_prefix_; // URL path component of base_url (e.g. /trade-api/v2)
};

} // namespace kalshi
