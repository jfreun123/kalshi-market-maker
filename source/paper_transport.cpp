#include "paper_transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <string>

namespace kalshi {

namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpCreated = 201;
constexpr int kOrdersPathSuffix = 7; // length of "/orders"

std::string order_to_json(const Order &order) {
  nlohmann::json json_order;
  json_order["order_id"] = order.id;
  json_order["ticker"] = order.market_ticker;
  json_order["side"] = (order.side == Side::Yes) ? "yes" : "no";
  json_order["count"] = order.quantity;
  json_order["filled_count"] = order.filled_quantity;
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
  return {kHttpOk, "{}"};
}

HttpResponse
PaperTransport::post(std::string_view /*url*/,
                     const std::map<std::string, std::string> & /*headers*/,
                     std::string_view body) {
  const nlohmann::json request_json = nlohmann::json::parse(body);

  const std::string ticker = request_json.at("ticker").get<std::string>();
  const std::string side_str = request_json.at("side").get<std::string>();
  const std::string type_str = request_json.at("type").get<std::string>();
  const int quantity = request_json.at("count").get<int>();

  const Side side = (side_str == "yes") ? Side::Yes : Side::No;
  const int price_cents = (side == Side::Yes)
                              ? request_json.at("yes_price").get<int>()
                              : request_json.at("no_price").get<int>();
  const OrderType order_type =
      (type_str == "limit") ? OrderType::Limit : OrderType::Market;

  Order order;
  order.id = next_order_id();
  order.market_ticker = ticker;
  order.side = side;
  order.price_cents = price_cents;
  order.quantity = quantity;
  order.filled_quantity = 0;
  order.status = OrderStatus::Open;
  order.type = order_type;
  order.created_at = std::chrono::system_clock::now();

  open_orders_.push_back(order);

  const std::string response_body = R"({"order":)" + order_to_json(order) + "}";
  return {kHttpCreated, response_body};
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

  const int remaining = order_iter->quantity - order_iter->filled_quantity;
  const int actual_fill = std::min(fill_quantity, remaining);
  order_iter->filled_quantity += actual_fill;
  order_iter->status = (order_iter->filled_quantity == order_iter->quantity)
                           ? OrderStatus::Filled
                           : OrderStatus::PartiallyFilled;

  Fill fill;
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
