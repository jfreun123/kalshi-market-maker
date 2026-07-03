#include "paper_transport.hpp"

#include "types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace kalshi {

namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpCreated = 201;
constexpr int kOrdersPathSuffix = 7; // length of "/orders"
constexpr double kCentsPerDollar = 100.0;
constexpr long long kPaperOrderTsMs = 1735689600000LL; // 2025-01-01T00:00:00Z

// Parses the V2 fixed-point wire strings RestClient sends back to integers:
// "0.5200" -> 52 cents, "10.00" -> 10 contracts.
int parse_dollars_to_cents(const std::string &dollars) {
  return static_cast<int>(std::lround(std::stod(dollars) * kCentsPerDollar));
}

int parse_count(const std::string &count) {
  return static_cast<int>(std::lround(std::stod(count)));
}

std::string order_to_json(const Order &order) {
  nlohmann::json json_order;
  json_order["order_id"] = order.id;
  json_order["ticker"] = order.market_ticker;
  json_order["side"] = (order.side == Side::Yes) ? "yes" : "no";
  json_order["count"] = order.quantity.to_fp_string();
  json_order["filled_count"] = order.filled_quantity.to_fp_string();
  json_order["type"] = (order.type == OrderType::Limit) ? "limit" : "market";
  json_order["created_time"] = "2025-01-01T00:00:00Z";

  if (order.side == Side::Yes) {
    json_order["yes_price"] = order.price_cents;
    json_order["no_price"] = 0;
  } else {
    json_order["yes_price"] = 0;
    json_order["no_price"] = order.price_cents;
  }

  switch (order.status) {
  case OrderStatus::Open:
    json_order["status"] = "resting";
    break;
  case OrderStatus::PartiallyFilled:
    json_order["status"] = "partially_filled";
    break;
  case OrderStatus::Filled:
    json_order["status"] = "filled";
    break;
  case OrderStatus::Cancelled:
    json_order["status"] = "canceled";
    break;
  }

  return json_order.dump();
}

bool url_ends_with_orders(std::string_view url) {
  return url.size() >= static_cast<std::size_t>(kOrdersPathSuffix) &&
         url.substr(url.size() - static_cast<std::size_t>(kOrdersPathSuffix)) ==
             "/orders";
}

} // namespace

HttpResponse
PaperTransport::get(std::string_view url,
                    const std::map<std::string, std::string> & /*headers*/) {
  if (url_ends_with_orders(url)) {
    nlohmann::json response_json;
    response_json["orders"] = nlohmann::json::array();
    for (const auto &order : open_orders_) {
      response_json["orders"].push_back(
          nlohmann::json::parse(order_to_json(order)));
    }
    return {kHttpOk, response_json.dump()};
  }
  if (url.find("/orderbook") != std::string_view::npos) {
    return {kHttpOk, R"({"orderbook_fp":{"yes_dollars":[],"no_dollars":[]}})"};
  }
  return {kHttpOk, "{}"};
}

HttpResponse
PaperTransport::post(std::string_view /*url*/,
                     const std::map<std::string, std::string> & /*headers*/,
                     std::string_view body) {
  // Parse the V2 create-order body that RestClient::place_order emits: side is
  // "bid"/"ask" in the YES dimension, price/count are fixed-point strings, and
  // time_in_force (not an order "type") distinguishes limit from IOC.
  const nlohmann::json request_json = nlohmann::json::parse(body);

  const std::string ticker = request_json.at("ticker").get<std::string>();
  const std::string v2_side = request_json.at("side").get<std::string>();
  const int yes_price_cents =
      parse_dollars_to_cents(request_json.at("price").get<std::string>());
  const int quantity = parse_count(request_json.at("count").get<std::string>());
  const std::string tif =
      request_json.value("time_in_force", std::string{"good_till_canceled"});

  const Side side = (v2_side == "bid") ? Side::Yes : Side::No;
  // Store the price in the order's own side dimension (NO = 100 - YES price).
  const int price_cents =
      (side == Side::Yes) ? yes_price_cents : complement_price(yes_price_cents);
  const OrderType order_type =
      (tif == "immediate_or_cancel") ? OrderType::Market : OrderType::Limit;

  Order order;
  order.id = next_order_id();
  order.market_ticker = ticker;
  order.side = side;
  order.price_cents = price_cents;
  order.quantity = Quantity::from_contracts(quantity);
  order.filled_quantity = Quantity{};
  order.status = OrderStatus::Open;
  order.type = order_type;
  order.created_at = std::chrono::system_clock::now();

  open_orders_.push_back(order);

  // V2 minimal response; RestClient reconstructs the Order from input params.
  nlohmann::json response_json;
  response_json["order_id"] = order.id;
  response_json["fill_count"] = "0.00";
  response_json["ts_ms"] = kPaperOrderTsMs;
  return {kHttpCreated, response_json.dump()};
}

HttpResponse PaperTransport::delete_(
    std::string_view url,
    const std::map<std::string, std::string> & /*headers*/) {
  // Extract order_id from the URL: /portfolio/orders/{order_id}
  const auto slash_pos = url.rfind('/');
  if (slash_pos != std::string_view::npos) {
    const std::string order_id{url.substr(slash_pos + 1U)};
    const auto order_iter = std::find_if(
        open_orders_.begin(), open_orders_.end(),
        [&order_id](const Order &order) { return order.id == order_id; });
    if (order_iter != open_orders_.end()) {
      open_orders_.erase(order_iter);
    }
  }
  return {kHttpOk, "{}"};
}

bool PaperTransport::simulate_fill(const std::string &order_id,
                                   int fill_quantity) {
  const auto order_iter =
      std::find_if(open_orders_.begin(), open_orders_.end(),
                   [&order_id](const Order &order) {
                     return order.id == order_id &&
                            order.status != OrderStatus::Filled &&
                            order.status != OrderStatus::Cancelled;
                   });

  if (order_iter == open_orders_.end()) {
    return false;
  }

  const Quantity remaining = order_iter->quantity - order_iter->filled_quantity;
  const Quantity actual_fill =
      kalshi::min(Quantity::from_contracts(fill_quantity), remaining);
  order_iter->filled_quantity += actual_fill;
  order_iter->status = (order_iter->filled_quantity == order_iter->quantity)
                           ? OrderStatus::Filled
                           : OrderStatus::PartiallyFilled;

  Fill fill;
  fill.trade_id = "paper-trade-" + std::to_string(next_id_++);
  fill.order_id = order_id;
  fill.market_ticker = order_iter->market_ticker;
  fill.side = order_iter->side;
  fill.price_cents = order_iter->price_cents;
  fill.quantity = actual_fill;
  fill.timestamp = std::chrono::system_clock::now();
  fills_.push_back(fill);

  if (order_iter->status == OrderStatus::Filled) {
    open_orders_.erase(order_iter);
  }

  return true;
}

const std::vector<Order> &PaperTransport::open_orders() const {
  return open_orders_;
}

const std::vector<Fill> &PaperTransport::fills() const { return fills_; }

std::string PaperTransport::next_order_id() {
  return "paper-" + std::to_string(next_id_++);
}

} // namespace kalshi
