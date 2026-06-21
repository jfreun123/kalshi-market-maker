#include "rest_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
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

// --- Fixed-point price/count conversion ---

// Converts an integer cent value to a fixed-point dollar string: 52 -> "0.5200"
constexpr double kCentsPerDollar = 100.0;

auto format_dollars(int cents) -> std::string {
  return std::format("{:.4f}", cents / kCentsPerDollar);
}

// Converts an integer contract count to a fixed-point string: 10 -> "10.00"
auto format_count(int count) -> std::string {
  return std::format("{:.2f}", static_cast<double>(count));
}

// Parses a fixed-point dollar string to integer cents: "0.5200" -> 52
auto parse_dollars_to_cents(const std::string &dollars) -> int {
  return static_cast<int>(std::round(std::stod(dollars) * kCentsPerDollar));
}

// Parses a fixed-point count string to integer: "10.00" -> 10
auto parse_fp_count(const std::string &fp_str) -> int {
  return static_cast<int>(std::round(std::stod(fp_str)));
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
  if (status_str == "resting") {
    return OrderStatus::Open;
  }
  if (status_str == "executed") {
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
      .title = market_json.value("title", std::string{}),
      // fee_rate_bps was removed from the Kalshi API schema; use if present.
      .fee_rate_bps = market_json.value("fee_rate_bps", 0),
      .close_time =
          parse_iso8601(market_json.at("close_time").get<std::string>()),
  };
}

// Parses an order from a GET /portfolio/orders response object.
// The V2 API uses outcome_side, yes_price_dollars/no_price_dollars,
// initial_count_fp, and fill_count_fp instead of the old integer fields.
Order parse_order(const nlohmann::json &order_json) {
  const auto side =
      parse_side(order_json.at("outcome_side").get<std::string>());
  const int price_cents =
      (side == Side::Yes)
          ? parse_dollars_to_cents(
                order_json.at("yes_price_dollars").get<std::string>())
          : parse_dollars_to_cents(
                order_json.at("no_price_dollars").get<std::string>());
  const int quantity =
      parse_fp_count(order_json.at("initial_count_fp").get<std::string>());
  const int filled_qty =
      parse_fp_count(order_json.at("fill_count_fp").get<std::string>());

  // The API no longer uses "partially_filled" as a status; a resting order
  // with fill_count_fp > 0 is a partial fill.
  auto status = parse_order_status(order_json.at("status").get<std::string>());
  if (status == OrderStatus::Open && filled_qty > 0) {
    status = OrderStatus::PartiallyFilled;
  }

  return Order{
      .id = order_json.at("order_id").get<std::string>(),
      .market_ticker = order_json.at("ticker").get<std::string>(),
      .side = side,
      .price_cents = price_cents,
      .quantity = quantity,
      .filled_quantity = filled_qty,
      .status = status,
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
  // The V2 API wraps the orderbook in "orderbook_fp" with string-valued arrays.
  const auto &orderbook_json = json_data.at("orderbook_fp");

  Orderbook result;
  result.ticker = ticker_str;
  for (const auto &level_array : orderbook_json.at("yes_dollars")) {
    const int price = parse_dollars_to_cents(level_array[0].get<std::string>());
    const int qty = parse_fp_count(level_array[1].get<std::string>());
    result.yes.push_back({price, qty});
  }
  for (const auto &level_array : orderbook_json.at("no_dollars")) {
    const int price = parse_dollars_to_cents(level_array[0].get<std::string>());
    const int qty = parse_fp_count(level_array[1].get<std::string>());
    result.no.push_back({price, qty});
  }
  return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - price and quantity are
// distinct
Order RestClient::place_order(std::string_view ticker, Side side,
                              int price_cents, int quantity, OrderType type) {
  // V2 create-order endpoint: POST /portfolio/events/orders
  const std::string path = path_prefix_ + "/portfolio/events/orders";
  const std::string url = base_url_ + "/portfolio/events/orders";

  auto headers = auth_.sign("POST", path);
  headers["Content-Type"] = "application/json";

  // V2 uses a single-book model: "bid" = buy YES, "ask" = sell YES (= buy NO).
  // All prices are in the YES dimension.
  constexpr int kMaxPriceCents = 100;
  const std::string v2_side = (side == Side::Yes) ? "bid" : "ask";
  const int yes_price_cents =
      (side == Side::Yes) ? price_cents : (kMaxPriceCents - price_cents);
  const std::string v2_tif = (type == OrderType::Market) ? "immediate_or_cancel"
                                                         : "good_till_canceled";

  nlohmann::json body_json;
  body_json["ticker"] = ticker;
  body_json["side"] = v2_side;
  body_json["price"] = format_dollars(yes_price_cents);
  body_json["count"] = format_count(quantity);
  body_json["time_in_force"] = v2_tif;
  body_json["self_trade_prevention_type"] = "taker_at_cross";

  auto resp = transport_->post(url, headers, body_json.dump());
  check_response(resp);

  // V2 returns a minimal response; reconstruct the Order from input params.
  auto json_data = nlohmann::json::parse(resp.body);
  const int filled_qty =
      parse_fp_count(json_data.at("fill_count").get<std::string>());

  OrderStatus status = OrderStatus::Open;
  if (filled_qty >= quantity) {
    status = OrderStatus::Filled;
  } else if (filled_qty > 0) {
    status = OrderStatus::PartiallyFilled;
  }

  // ts_ms is Unix epoch milliseconds.
  const auto ts_ms = json_data.at("ts_ms").get<long long>();
  const auto created_at =
      std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms));

  return Order{
      .id = json_data.at("order_id").get<std::string>(),
      .market_ticker = std::string(ticker),
      .side = side,
      .price_cents = price_cents,
      .quantity = quantity,
      .filled_quantity = filled_qty,
      .status = status,
      .type = type,
      .created_at = created_at,
  };
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
