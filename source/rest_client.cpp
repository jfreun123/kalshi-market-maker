#include "rest_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <stdexcept>
#include <string>

namespace kalshi {

namespace {

// --- URL helpers ---

// Extracts the path component from a URL.
// "https://host.com/trade-api/v2" -> "/trade-api/v2"
std::string extract_path_prefix(const std::string &url) {
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return "";
  }
  auto path_start = url.find('/', scheme_end + 3U);
  if (path_start == std::string::npos) {
    return "";
  }
  return url.substr(path_start);
}

// --- HTTP response validation ---

void check_response(const HttpResponse &resp) {
  constexpr int kHttpSuccessMin = 200;
  constexpr int kHttpSuccessMax = 299;
  if (resp.status_code < kHttpSuccessMin ||
      resp.status_code > kHttpSuccessMax) {
    throw std::runtime_error("HTTP " + std::to_string(resp.status_code) + ": " +
                             resp.body);
  }
}

// --- ISO 8601 timestamp parsing ---

// Parses "2025-01-01T00:00:00Z" into a system_clock time_point.
auto parse_iso8601(const std::string &time_str)
    -> std::chrono::system_clock::time_point {
  struct std::tm time_struct {};
  // NOLINTNEXTLINE(modernize-use-nullptr) — strptime returns char*
  if (strptime(time_str.c_str(), "%Y-%m-%dT%H:%M:%SZ", &time_struct) ==
      nullptr) {
    throw std::runtime_error("Failed to parse timestamp: " + time_str);
  }
  time_struct.tm_isdst = 0;
  // NOLINTNEXTLINE(concurrency-mt-unsafe) — single-threaded; timegm is
  // reentrant on Linux
  std::time_t epoch = timegm(&time_struct);
  return std::chrono::system_clock::from_time_t(epoch);
}

// --- Enum parsers ---

Side parse_side(const std::string &side_str) {
  if (side_str == "yes") {
    return Side::Yes;
  }
  if (side_str == "no") {
    return Side::No;
  }
  throw std::runtime_error("Unknown side: " + side_str);
}

OrderStatus parse_order_status(const std::string &status_str) {
  if (status_str == "resting" || status_str == "pending" ||
      status_str == "open") {
    return OrderStatus::Open;
  }
  if (status_str == "partially_filled") {
    return OrderStatus::PartiallyFilled;
  }
  if (status_str == "filled" || status_str == "executed") {
    return OrderStatus::Filled;
  }
  if (status_str == "canceled" || status_str == "cancelled") {
    return OrderStatus::Cancelled;
  }
  throw std::runtime_error("Unknown order status: " + status_str);
}

OrderType parse_order_type(const std::string &type_str) {
  if (type_str == "limit") {
    return OrderType::Limit;
  }
  if (type_str == "market") {
    return OrderType::Market;
  }
  throw std::runtime_error("Unknown order type: " + type_str);
}

// --- JSON struct parsers ---

Market parse_market(const nlohmann::json &market_json) {
  return Market{
      .ticker = market_json.at("ticker").get<std::string>(),
      .title = market_json.at("title").get<std::string>(),
      .fee_rate_bps = market_json.at("fee_rate_bps").get<int>(),
      .close_time =
          parse_iso8601(market_json.at("close_time").get<std::string>()),
  };
}

Order parse_order(const nlohmann::json &order_json) {
  auto side = parse_side(order_json.at("side").get<std::string>());
  int price_cents = (side == Side::Yes) ? order_json.at("yes_price").get<int>()
                                        : order_json.at("no_price").get<int>();

  return Order{
      .id = order_json.at("order_id").get<std::string>(),
      .market_ticker = order_json.at("ticker").get<std::string>(),
      .side = side,
      .price_cents = price_cents,
      .quantity = order_json.at("count").get<int>(),
      .filled_quantity = order_json.at("filled_count").get<int>(),
      .status = parse_order_status(order_json.at("status").get<std::string>()),
      .type = parse_order_type(order_json.at("type").get<std::string>()),
      .created_at =
          parse_iso8601(order_json.at("created_time").get<std::string>()),
  };
}

} // namespace

// --- RestClient implementation ---

RestClient::RestClient(Auth auth, std::unique_ptr<IHttpTransport> transport,
                       std::string base_url)
    : auth_{std::move(auth)}, transport_{std::move(transport)},
      base_url_{std::move(base_url)},
      path_prefix_{extract_path_prefix(base_url_)} {}

std::vector<Market> RestClient::get_markets(std::string_view event_ticker) {
  std::string path = path_prefix_ + "/markets";
  std::string url = base_url_ + "/markets";
  if (!event_ticker.empty()) {
    url += "?event_ticker=" + std::string(event_ticker);
  }

  auto headers = auth_.sign("GET", path);
  auto resp = transport_->get(url, headers);
  check_response(resp);

  auto json_data = nlohmann::json::parse(resp.body);
  std::vector<Market> markets;
  for (const auto &market_json : json_data.at("markets")) {
    markets.push_back(parse_market(market_json));
  }
  return markets;
}

Orderbook RestClient::get_orderbook(std::string_view ticker) {
  std::string ticker_str{ticker};
  std::string path = path_prefix_ + "/markets/" + ticker_str + "/orderbook";
  std::string url = base_url_ + "/markets/" + ticker_str + "/orderbook";

  auto headers = auth_.sign("GET", path);
  auto resp = transport_->get(url, headers);
  check_response(resp);

  auto json_data = nlohmann::json::parse(resp.body);
  const auto &orderbook_json = json_data.at("orderbook");

  Orderbook result;
  result.ticker = ticker_str;
  for (const auto &level_array : orderbook_json.at("yes")) {
    result.yes.push_back(
        {level_array[0].get<int>(), level_array[1].get<int>()});
  }
  for (const auto &level_array : orderbook_json.at("no")) {
    result.no.push_back({level_array[0].get<int>(), level_array[1].get<int>()});
  }
  return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — price_cents and
// quantity are semantically distinct
Order RestClient::place_order(std::string_view ticker, Side side,
                              int price_cents, int quantity, OrderType type) {
  std::string path = path_prefix_ + "/portfolio/orders";
  std::string url = base_url_ + "/portfolio/orders";

  auto headers = auth_.sign("POST", path);
  headers["Content-Type"] = "application/json";

  nlohmann::json body_json;
  body_json["ticker"] = ticker;
  body_json["action"] = "buy";
  body_json["side"] = (side == Side::Yes) ? "yes" : "no";
  body_json["type"] = (type == OrderType::Limit) ? "limit" : "market";
  body_json["count"] = quantity;
  if (side == Side::Yes) {
    body_json["yes_price"] = price_cents;
  } else {
    body_json["no_price"] = price_cents;
  }

  auto resp = transport_->post(url, headers, body_json.dump());
  check_response(resp);

  auto json_data = nlohmann::json::parse(resp.body);
  return parse_order(json_data.at("order"));
}

bool RestClient::cancel_order(std::string_view order_id) {
  std::string order_id_str{order_id};
  std::string path = path_prefix_ + "/portfolio/orders/" + order_id_str;
  std::string url = base_url_ + "/portfolio/orders/" + order_id_str;

  auto headers = auth_.sign("DELETE", path);
  auto resp = transport_->delete_(url, headers);

  constexpr int kHttpSuccessMin = 200;
  constexpr int kHttpSuccessMax = 299;
  return resp.status_code >= kHttpSuccessMin &&
         resp.status_code <= kHttpSuccessMax;
}

std::vector<Order> RestClient::get_open_orders() {
  std::string path = path_prefix_ + "/portfolio/orders";
  std::string url = base_url_ + "/portfolio/orders?status=resting";

  auto headers = auth_.sign("GET", path);
  auto resp = transport_->get(url, headers);
  check_response(resp);

  auto json_data = nlohmann::json::parse(resp.body);
  std::vector<Order> orders;
  for (const auto &order_json : json_data.at("orders")) {
    orders.push_back(parse_order(order_json));
  }
  return orders;
}

} // namespace kalshi
