#include "rest_client.hpp"

#include "logger.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <stdexcept>
#include <string>
#include <thread>

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
  if (strptime(time_str.c_str(), "%Y-%m-%dT%H:%M:%S", &time_struct) ==
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

// Parses a fixed-point dollar string to integer cents: "0.5200" -> 52
auto parse_dollars_to_cents(const std::string &dollars) -> int {
  return static_cast<int>(std::round(std::stod(dollars) * kCentsPerDollar));
}

auto parse_fp_count(const std::string &fp_str) -> Quantity {
  return Quantity::from_fp_string(fp_str);
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
  const int yes_bid = parse_dollars_to_cents(
      market_json.value("yes_bid_dollars", std::string{"0.0000"}));
  const int yes_ask = parse_dollars_to_cents(
      market_json.value("yes_ask_dollars", std::string{"0.0000"}));
  return Market{
      .ticker = market_json.at("ticker").get<std::string>(),
      .title = market_json.value("title", std::string{}),
      .category = market_json.value("category", std::string{}),
      .status = market_json.value("status", std::string{}),
      // fee_rate_bps was removed from the Kalshi API schema; use if present.
      .fee_rate_bps = market_json.value("fee_rate_bps", 0),
      .yes_bid_cents = yes_bid,
      .yes_ask_cents = yes_ask,
      .volume_24h =
          market_json.contains("volume_24h_fp")
              ? std::stod(market_json.at("volume_24h_fp").get<std::string>())
              : 0.0,
      .close_time =
          parse_iso8601(market_json.at("close_time").get<std::string>()),
  };
}

// Parses a dollars fixed-point string to a (possibly fractional) cents value.
double parse_dollars_to_cents_d(const std::string &dollars) {
  return std::stod(dollars) * kCentsPerDollar;
}

MarketPosition parse_position(const nlohmann::json &position_json) {
  return MarketPosition{
      .ticker = position_json.at("ticker").get<std::string>(),
      .position = parse_fp_count(
          position_json.value("position_fp", std::string{"0.00"})),
      .realized_pnl_cents = parse_dollars_to_cents_d(
          position_json.value("realized_pnl_dollars", std::string{"0"})),
      .market_exposure_cents = parse_dollars_to_cents_d(
          position_json.value("market_exposure_dollars", std::string{"0"})),
      .resting_orders_count = position_json.value("resting_orders_count", 0),
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
  const Quantity quantity =
      parse_fp_count(order_json.at("initial_count_fp").get<std::string>());
  const Quantity filled_qty =
      parse_fp_count(order_json.at("fill_count_fp").get<std::string>());

  auto status = parse_order_status(order_json.at("status").get<std::string>());
  if (status == OrderStatus::Open && filled_qty.is_positive()) {
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

// Parses a fill from a GET /portfolio/fills response object. Mirrors the WS
// fill-channel parse: price stored side-native, `ts` is epoch seconds (the WS
// channel sends ts_ms).
Fill parse_fill(const nlohmann::json &fill_json) {
  Fill fill;
  fill.trade_id = fill_json.value("trade_id", std::string{});
  fill.order_id = fill_json.at("order_id").get<std::string>();
  fill.market_ticker = fill_json.at("market_ticker").get<std::string>();
  fill.side = parse_side(fill_json.at("outcome_side").get<std::string>());
  const int yes_price_cents = parse_dollars_to_cents(
      fill_json.at("yes_price_dollars").get<std::string>());
  fill.price_cents = (fill.side == Side::Yes)
                         ? yes_price_cents
                         : complement_price(yes_price_cents);
  fill.quantity = parse_fp_count(fill_json.at("count_fp").get<std::string>());
  fill.fee_cents = std::stod(fill_json.value("fee_cost", std::string{"0"})) *
                   kCentsPerDollar;
  fill.is_taker = fill_json.at("is_taker").get<bool>();
  fill.timestamp = std::chrono::system_clock::time_point{
      std::chrono::seconds{fill_json.at("ts").get<long long>()}};
  return fill;
}

} // namespace

// --- RestClient implementation ---

namespace {
constexpr double kBasicWriteTokensPerSecond = 100.0;
constexpr double kBasicWriteCapacity = 100.0;
constexpr double kPlaceOrderCost = 10.0;
constexpr double kCancelOrderCost = 2.0;
} // namespace

RestClient::RestClient(Auth auth, std::unique_ptr<IHttpTransport> transport,
                       std::string base_url)
    : auth_{std::move(auth)}, transport_{std::move(transport)},
      base_url_{std::move(base_url)},
      path_prefix_{extract_path_prefix(base_url_)},
      write_limiter_{kBasicWriteTokensPerSecond, kBasicWriteCapacity} {}

std::vector<Market> RestClient::get_markets(std::string_view event_ticker) {
  // Max page size accepted by the Kalshi /markets endpoint. Larger pages mean
  // fewer round-trips when paginating the full (150k+ market) listing.
  constexpr int kMarketsPageLimit = 1000;

  const std::string path = path_prefix_ + "/markets";
  // status=open restricts to currently-tradeable markets; without it the
  // listing includes the entire settled archive (150k+ markets).
  std::string base_query =
      "limit=" + std::to_string(kMarketsPageLimit) + "&status=open";
  if (!event_ticker.empty()) {
    base_query += "&event_ticker=" + std::string(event_ticker);
  }

  // Log progress every N pages so a long full-listing scan doesn't look hung.
  constexpr int kProgressEveryPages = 10;

  std::vector<Market> all_markets;
  std::string cursor;
  int pages = 0;

  auto fetch_page = [&]() {
    std::string url = base_url_ + "/markets?" + base_query;
    if (!cursor.empty()) {
      url += "&cursor=" + cursor;
    }

    constexpr int kPageAttempts = 3;
    constexpr auto kPageRetryDelay = std::chrono::milliseconds{300};
    HttpResponse resp;
    for (int attempt = 1;; ++attempt) {
      try {
        auto headers = auth_.sign("GET", path);
        resp = transport_->get(url, headers);
        check_response(resp);
        break;
      } catch (const std::exception &ex) {
        if (attempt >= kPageAttempts) {
          throw;
        }
        get_logger()->warn(
            "get_markets page fetch failed (attempt {}/{}): {} — retrying",
            attempt, kPageAttempts, ex.what());
        std::this_thread::sleep_for(kPageRetryDelay);
      }
    }

    auto json_data = nlohmann::json::parse(resp.body);
    for (const auto &market_json : json_data.at("markets")) {
      all_markets.push_back(parse_market(market_json));
    }
    cursor = json_data.value("cursor", std::string{});
    ++pages;
    if (pages % kProgressEveryPages == 0) {
      get_logger()->info("get_markets progress: {} pages, {} markets fetched",
                         pages, all_markets.size());
    }
  };

  fetch_page(); // first page (cursor is empty — server returns from beginning)
  while (!cursor.empty()) {
    fetch_page();
  }

  return all_markets;
}

std::vector<MarketPosition> RestClient::get_positions() {
  const std::string path = path_prefix_ + "/portfolio/positions";

  std::vector<MarketPosition> all_positions;
  std::string cursor;

  auto fetch_page = [&]() {
    std::string url = base_url_ + "/portfolio/positions";
    if (!cursor.empty()) {
      url += "?cursor=" + cursor;
    }

    auto headers = auth_.sign("GET", path);
    auto resp = transport_->get(url, headers);
    check_response(resp);

    auto json_data = nlohmann::json::parse(resp.body);
    for (const auto &position_json : json_data.at("market_positions")) {
      all_positions.push_back(parse_position(position_json));
    }
    cursor = json_data.value("cursor", std::string{});
  };

  fetch_page();
  while (!cursor.empty()) {
    fetch_page();
  }

  return all_positions;
}

std::vector<IncentiveProgram> RestClient::get_incentive_programs() {
  constexpr int kPageLimit = 1000;
  const std::string path = path_prefix_ + "/incentive_programs";
  const std::string base_query =
      "status=active&type=liquidity&limit=" + std::to_string(kPageLimit);

  std::vector<IncentiveProgram> all_programs;
  std::string cursor;

  auto fetch_page = [&]() {
    std::string url = base_url_ + "/incentive_programs?" + base_query;
    if (!cursor.empty()) {
      url += "&cursor=" + cursor;
    }
    auto headers = auth_.sign("GET", path);
    auto resp = transport_->get(url, headers);
    check_response(resp);

    auto json_data = nlohmann::json::parse(resp.body);
    static const nlohmann::json kEmptyArray = nlohmann::json::array();
    for (const auto &program_json :
         json_data.value("incentive_programs", kEmptyArray)) {
      IncentiveProgram program;
      program.market_ticker =
          program_json.value("market_ticker", std::string{});
      if (const auto reward = program_json.find("period_reward");
          reward != program_json.end() && reward->is_number()) {
        program.period_reward_centicents = reward->get<long long>();
      }
      if (const auto target = program_json.find("target_size_fp");
          target != program_json.end() && target->is_string()) {
        program.target_size = parse_fp_count(target->get<std::string>());
      }
      if (const auto discount = program_json.find("discount_factor_bps");
          discount != program_json.end() && discount->is_number()) {
        program.discount_factor_bps = discount->get<int>();
      }
      all_programs.push_back(std::move(program));
    }
    cursor = json_data.value("next_cursor",
                             json_data.value("cursor", std::string{}));
  };

  fetch_page();
  while (!cursor.empty()) {
    fetch_page();
  }

  return all_programs;
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
    const Quantity qty = parse_fp_count(level_array[1].get<std::string>());
    result.yes.push_back({price, qty});
  }
  for (const auto &level_array : orderbook_json.at("no_dollars")) {
    const int price = parse_dollars_to_cents(level_array[0].get<std::string>());
    const Quantity qty = parse_fp_count(level_array[1].get<std::string>());
    result.no.push_back({price, qty});
  }
  return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - price and quantity are
// distinct
Order RestClient::place_order(std::string_view ticker, Side side,
                              int price_cents, int quantity, OrderType type) {
  return place_order(ticker, side, price_cents,
                     Quantity::from_contracts(quantity), type);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - price and count
// distinct
Order RestClient::place_order(std::string_view ticker, Side side,
                              int price_cents, Quantity count, OrderType type) {
  // V2 create-order endpoint: POST /portfolio/events/orders
  const std::string path = path_prefix_ + "/portfolio/events/orders";
  const std::string url = base_url_ + "/portfolio/events/orders";

  auto headers = auth_.sign("POST", path);
  headers["Content-Type"] = "application/json";

  // V2 uses a single-book model: "bid" = buy YES, "ask" = sell YES (= buy NO).
  // All prices are in the YES dimension.
  const std::string v2_side = (side == Side::Yes) ? "bid" : "ask";
  const int yes_price_cents =
      (side == Side::Yes) ? price_cents : (kPriceBasis - price_cents);
  const std::string v2_tif = (type == OrderType::Market) ? "immediate_or_cancel"
                                                         : "good_till_canceled";

  nlohmann::json body_json;
  body_json["ticker"] = ticker;
  body_json["side"] = v2_side;
  body_json["price"] = format_dollars(yes_price_cents);
  body_json["count"] = count.to_fp_string();
  body_json["time_in_force"] = v2_tif;
  body_json["self_trade_prevention_type"] = "taker_at_cross";
  if (v2_tif == "good_till_canceled") {
    body_json["post_only"] = true;
    body_json["cancel_order_on_pause"] = true;
  }

  std::this_thread::sleep_for(write_limiter_.acquire(kPlaceOrderCost));
  auto resp = transport_->post(url, headers, body_json.dump());
  check_response(resp);

  // V2 returns a minimal response; reconstruct the Order from input params.
  auto json_data = nlohmann::json::parse(resp.body);
  const Quantity filled_qty =
      parse_fp_count(json_data.at("fill_count").get<std::string>());

  OrderStatus status = OrderStatus::Open;
  if (filled_qty >= count) {
    status = OrderStatus::Filled;
  } else if (type == OrderType::Market) {
    status = OrderStatus::Cancelled;
  } else if (filled_qty.is_positive()) {
    status = OrderStatus::PartiallyFilled;
  }

  // ts_ms is Unix epoch milliseconds.
  const auto ts_ms = json_data.at("ts_ms").get<long long>();
  const auto created_at =
      std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms));

  // average_fill_price is the YES-leg VWAP, present only when fill_count > 0;
  // store it side-native to match the Fill price convention.
  int avg_fill_cents = 0;
  const auto avg_str = json_data.value("average_fill_price", std::string{});
  if (!avg_str.empty()) {
    const int avg_yes_cents = parse_dollars_to_cents(avg_str);
    avg_fill_cents =
        (side == Side::Yes) ? avg_yes_cents : complement_price(avg_yes_cents);
  }

  return Order{
      .id = json_data.at("order_id").get<std::string>(),
      .market_ticker = std::string(ticker),
      .side = side,
      .price_cents = price_cents,
      .quantity = count,
      .filled_quantity = filled_qty,
      .status = status,
      .type = type,
      .created_at = created_at,
      .average_fill_price_cents = avg_fill_cents,
  };
}

Order RestClient::flatten(std::string_view ticker, Quantity net_position) {
  if (net_position.is_zero()) {
    return Order{};
  }
  // Long YES (net > 0) closes by selling YES (Side::No = "ask"); long NO closes
  // by buying YES (Side::Yes = "bid"). Price the close at the max cent so the
  // immediate-or-cancel order crosses and fills at the resting price.
  const Side close_side = net_position.is_positive() ? Side::No : Side::Yes;
  const Quantity count =
      net_position.is_positive() ? net_position : -net_position;
  return place_order(ticker, close_side, kMaxPriceCents, count,
                     OrderType::Market);
}

bool RestClient::cancel_order(std::string_view order_id) {
  std::string order_id_str{order_id};
  std::string path = path_prefix_ + "/portfolio/events/orders/" + order_id_str;
  std::string url = base_url_ + "/portfolio/events/orders/" + order_id_str;

  auto headers = auth_.sign("DELETE", path);
  std::this_thread::sleep_for(write_limiter_.acquire(kCancelOrderCost));
  auto resp = transport_->delete_(url, headers);

  constexpr int kHttpSuccessMin = 200;
  constexpr int kHttpSuccessMax = 299;
  return resp.status_code >= kHttpSuccessMin &&
         resp.status_code <= kHttpSuccessMax;
}

std::optional<std::string>
RestClient::amend_order(std::string_view order_id, std::string_view ticker,
                        Side side, int new_price_cents, Quantity count) {
  const std::string id_str{order_id};
  const std::string path =
      path_prefix_ + "/portfolio/events/orders/" + id_str + "/amend";
  const std::string url =
      base_url_ + "/portfolio/events/orders/" + id_str + "/amend";
  auto headers = auth_.sign("POST", path);
  headers["Content-Type"] = "application/json";

  const std::string v2_side = (side == Side::Yes) ? "bid" : "ask";
  const int yes_price_cents =
      (side == Side::Yes) ? new_price_cents : (kPriceBasis - new_price_cents);
  nlohmann::json body_json;
  body_json["ticker"] = ticker;
  body_json["side"] = v2_side;
  body_json["price"] = format_dollars(yes_price_cents);
  body_json["count"] = count.to_fp_string();
  body_json["time_in_force"] = "good_till_canceled";
  body_json["self_trade_prevention_type"] = "taker_at_cross";
  body_json["post_only"] = true;

  std::this_thread::sleep_for(write_limiter_.acquire(kPlaceOrderCost));
  auto resp = transport_->post(url, headers, body_json.dump());
  try {
    check_response(resp);
    auto json_data = nlohmann::json::parse(resp.body);
    return json_data.value("order_id", id_str);
  } catch (const std::exception &ex) {
    get_logger()->warn("amend failed order_id={}: {}", order_id, ex.what());
    return std::nullopt;
  }
}

std::optional<Quantity> RestClient::decrease_order(std::string_view order_id,
                                                   Quantity reduce_to) {
  const std::string id_str{order_id};
  const std::string path =
      path_prefix_ + "/portfolio/events/orders/" + id_str + "/decrease";
  const std::string url =
      base_url_ + "/portfolio/events/orders/" + id_str + "/decrease";
  auto headers = auth_.sign("POST", path);
  headers["Content-Type"] = "application/json";
  nlohmann::json body_json;
  body_json["reduce_to"] = reduce_to.to_fp_string();

  std::this_thread::sleep_for(write_limiter_.acquire(kCancelOrderCost));
  auto resp = transport_->post(url, headers, body_json.dump());
  try {
    check_response(resp);
    auto json_data = nlohmann::json::parse(resp.body);
    return parse_fp_count(json_data.at("remaining_count").get<std::string>());
  } catch (const std::exception &ex) {
    get_logger()->warn("decrease failed order_id={}: {}", order_id, ex.what());
    return std::nullopt;
  }
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

std::optional<std::vector<PublicTrade>>
RestClient::get_recent_trades(std::string_view ticker, int limit) {
  const std::string path = path_prefix_ + "/markets/trades";
  const std::string url = base_url_ +
                          "/markets/trades?limit=" + std::to_string(limit) +
                          "&ticker=" + std::string{ticker};
  try {
    auto headers = auth_.sign("GET", path);
    auto resp = transport_->get(url, headers);
    check_response(resp);
    auto json_data = nlohmann::json::parse(resp.body);
    std::vector<PublicTrade> trades;
    for (const auto &trade : json_data.at("trades")) {
      PublicTrade parsed;
      parsed.trade_id = trade.value("trade_id", std::string{});
      parsed.market_ticker = trade.value("ticker", std::string{ticker});
      parsed.yes_price_cents = parse_dollars_to_cents(
          trade.at("yes_price_dollars").get<std::string>());
      parsed.quantity = Quantity::from_fp_string(trade.value("count_fp", "0"));
      const std::string taker_side_str =
          trade.contains("taker_outcome_side")
              ? trade.at("taker_outcome_side").get<std::string>()
              : trade.value("taker_side", "yes");
      parsed.taker_side = parse_side(taker_side_str);
      parsed.timestamp =
          parse_iso8601(trade.at("created_time").get<std::string>());
      trades.push_back(parsed);
    }
    return trades;
  } catch (const std::exception &ex) {
    get_logger()->debug("recent-trades probe failed ticker={}: {}", ticker,
                        ex.what());
    return std::nullopt;
  }
}

std::optional<std::vector<Candle>>
RestClient::get_candlesticks(std::string_view ticker,
                             long long start_ts_seconds,
                             long long end_ts_seconds) {
  const std::string ticker_str{ticker};
  const std::string series = ticker_str.substr(0, ticker_str.find('-'));
  const std::string endpoint =
      "/series/" + series + "/markets/" + ticker_str + "/candlesticks";
  const std::string path = path_prefix_ + endpoint;
  const std::string url =
      base_url_ + endpoint + "?start_ts=" + std::to_string(start_ts_seconds) +
      "&end_ts=" + std::to_string(end_ts_seconds) + "&period_interval=1";
  try {
    auto headers = auth_.sign("GET", path);
    auto resp = transport_->get(url, headers);
    check_response(resp);
    auto json_data = nlohmann::json::parse(resp.body);
    std::vector<Candle> candles;
    for (const auto &candle : json_data.at("candlesticks")) {
      Candle parsed;
      const auto price = candle.value("price", nlohmann::json::object());
      if (price.contains("close_dollars") &&
          !price.at("close_dollars").is_null()) {
        parsed.close_cents = parse_dollars_to_cents(
            price.at("close_dollars").get<std::string>());
      }
      candles.push_back(parsed);
    }
    return candles;
  } catch (const std::exception &ex) {
    get_logger()->debug("candlesticks probe failed ticker={}: {}", ticker,
                        ex.what());
    return std::nullopt;
  }
}

std::vector<Fill> RestClient::get_fills(long long min_ts_seconds) {
  constexpr int kPageLimit = 100;
  const std::string path = path_prefix_ + "/portfolio/fills";
  std::string base_query = "limit=" + std::to_string(kPageLimit);
  if (min_ts_seconds > 0) {
    base_query += "&min_ts=" + std::to_string(min_ts_seconds);
  }

  std::vector<Fill> all_fills;
  std::string cursor;

  auto fetch_page = [&]() {
    std::string url = base_url_ + "/portfolio/fills?" + base_query;
    if (!cursor.empty()) {
      url += "&cursor=" + cursor;
    }

    auto headers = auth_.sign("GET", path);
    auto resp = transport_->get(url, headers);
    check_response(resp);

    auto json_data = nlohmann::json::parse(resp.body);
    for (const auto &fill_json : json_data.at("fills")) {
      all_fills.push_back(parse_fill(fill_json));
    }
    cursor = json_data.value("cursor", std::string{});
  };

  fetch_page();
  while (!cursor.empty()) {
    fetch_page();
  }

  return all_fills;
}

} // namespace kalshi
