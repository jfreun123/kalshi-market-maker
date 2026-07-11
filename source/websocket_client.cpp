#include "websocket_client.hpp"
#include "auth.hpp"
#include "logger.hpp"
#include "types.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace kalshi {

// ---- IxWebSocket — delegates to ix::WebSocket ----

struct IxWebSocket::Impl {
  ix::WebSocket ws;
  MessageHandler on_message_cb;
  ConnectHandler on_connect_cb;
  DisconnectHandler on_disconnect_cb;
  HeartbeatHandler on_heartbeat_cb;
  std::mutex run_mtx;
  std::condition_variable run_cv;
  bool done{false};
};

IxWebSocket::IxWebSocket() : impl_{std::make_unique<Impl>()} {
  // Let WebSocketClient handle reconnect logic; IxWebSocket's internal
  // auto-reconnect would reuse stale auth timestamps.
  impl_->ws.disableAutomaticReconnection();

  constexpr int kIdlePingIntervalSeconds = 10;
  impl_->ws.setPingInterval(kIdlePingIntervalSeconds);

  impl_->ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
    switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      if (impl_->on_connect_cb) {
        impl_->on_connect_cb();
      }
      break;
    case ix::WebSocketMessageType::Close:
      if (impl_->on_disconnect_cb) {
        impl_->on_disconnect_cb();
      }
      {
        std::lock_guard<std::mutex> lock(impl_->run_mtx);
        impl_->done = true;
      }
      impl_->run_cv.notify_one();
      break;
    case ix::WebSocketMessageType::Error:
      get_logger()->warn("websocket error: {}", msg->errorInfo.reason);
      if (impl_->on_disconnect_cb) {
        impl_->on_disconnect_cb();
      }
      {
        std::lock_guard<std::mutex> lock(impl_->run_mtx);
        impl_->done = true;
      }
      impl_->run_cv.notify_one();
      break;
    case ix::WebSocketMessageType::Message:
      if (impl_->on_message_cb) {
        impl_->on_message_cb(msg->str);
      }
      break;
    case ix::WebSocketMessageType::Ping:
    case ix::WebSocketMessageType::Pong:
      if (impl_->on_heartbeat_cb) {
        impl_->on_heartbeat_cb();
      }
      break;
    default:
      break;
    }
  });
}

IxWebSocket::~IxWebSocket() = default;

void IxWebSocket::connect(std::string_view url,
                          const std::map<std::string, std::string> &headers) {
  impl_->ws.stop(); // reset internal state before applying new URL/headers
  impl_->ws.setUrl(std::string(url));
  const ix::WebSocketHttpHeaders ix_headers(headers.begin(), headers.end());
  impl_->ws.setExtraHeaders(ix_headers);
}

void IxWebSocket::send(const std::string &message) { impl_->ws.send(message); }

void IxWebSocket::on_message(MessageHandler handler) {
  impl_->on_message_cb = std::move(handler);
}

void IxWebSocket::on_connect(ConnectHandler handler) {
  impl_->on_connect_cb = std::move(handler);
}

void IxWebSocket::on_disconnect(DisconnectHandler handler) {
  impl_->on_disconnect_cb = std::move(handler);
}

void IxWebSocket::on_heartbeat(HeartbeatHandler handler) {
  impl_->on_heartbeat_cb = std::move(handler);
}

void IxWebSocket::run() {
  {
    std::lock_guard<std::mutex> lock(impl_->run_mtx);
    impl_->done = false;
  }
  impl_->ws.start();
  std::unique_lock<std::mutex> lock(impl_->run_mtx);
  impl_->run_cv.wait(lock, [this] { return impl_->done; });
}

void IxWebSocket::stop() {
  impl_->ws.stop();
  {
    std::lock_guard<std::mutex> lock(impl_->run_mtx);
    impl_->done = true;
  }
  impl_->run_cv.notify_one();
}

void IxWebSocket::request_close() { impl_->ws.close(); }

std::chrono::milliseconds
reconnect_backoff(std::chrono::milliseconds base_delay,
                  int consecutive_failures) {
  constexpr int kMaxDoublings = 30;
  const auto cap =
      std::chrono::duration_cast<std::chrono::milliseconds>(kMaxReconnectDelay);
  auto delay = std::min(base_delay, cap);
  const int doublings = std::min(consecutive_failures - 1, kMaxDoublings);
  for (int i = 0; i < doublings; ++i) {
    delay *= 2;
    if (delay >= cap) {
      return cap;
    }
  }
  return delay;
}

// ---- WebSocketClient helpers ----

namespace {

// Extracts the path portion of a WebSocket URL for auth signing.
// "wss://host.com/trade-api/ws/v2" -> "/trade-api/ws/v2"
std::string extract_ws_path(const std::string &url) {
  const std::string kWssPrefix = "wss://";
  const std::string kWsPrefix = "ws://";
  std::string_view rest = url;
  if (rest.starts_with(kWssPrefix)) {
    rest.remove_prefix(kWssPrefix.size());
  } else if (rest.starts_with(kWsPrefix)) {
    rest.remove_prefix(kWsPrefix.size());
  }
  const auto slash_pos = rest.find('/');
  if (slash_pos == std::string_view::npos) {
    return "/";
  }
  return std::string(rest.substr(slash_pos));
}

constexpr double kCentsPerDollar = 100.0;

// "0.5200" -> 52
int dollars_to_cents(const std::string &dollars_str) {
  return static_cast<int>(std::round(std::stod(dollars_str) * kCentsPerDollar));
}

Quantity parse_fp_count(const std::string &fp_str) {
  return Quantity::from_fp_string(fp_str);
}

Side parse_side(const std::string &side_str) {
  if (side_str == "yes") {
    return Side::Yes;
  }
  if (side_str == "no") {
    return Side::No;
  }
  throw std::runtime_error("ws: unknown side: " + side_str);
}

Level parse_snapshot_level(const nlohmann::json &level) {
  constexpr std::size_t kPriceField = 0;
  constexpr std::size_t kCountField = 1;
  return {dollars_to_cents(level.at(kPriceField).get<std::string>()),
          parse_fp_count(level.at(kCountField).get<std::string>())};
}

Orderbook parse_snapshot(const nlohmann::json &msg) {
  Orderbook book;
  book.ticker = msg.at("market_ticker").get<std::string>();

  static const nlohmann::json kEmptyArray = nlohmann::json::array();
  for (const auto &level : msg.value("yes_dollars_fp", kEmptyArray)) {
    book.yes.push_back(parse_snapshot_level(level));
  }
  for (const auto &level : msg.value("no_dollars_fp", kEmptyArray)) {
    book.no.push_back(parse_snapshot_level(level));
  }
  return book;
}

} // namespace

// ---- WebSocketClient ----

WebSocketClient::WebSocketClient(Auth auth,
                                 std::unique_ptr<IWebSocket> ws_transport,
                                 std::string base_url, int max_reconnects,
                                 std::chrono::milliseconds reconnect_delay)
    : auth_{std::move(auth)}, ws_{std::move(ws_transport)},
      ws_url_{std::move(base_url)}, max_reconnects_{max_reconnects},
      reconnect_delay_{reconnect_delay} {}

void WebSocketClient::subscribe(std::string_view ticker) {
  subscribed_tickers_.emplace_back(ticker);
}

void WebSocketClient::on_orderbook_snapshot(SnapshotCallback callback) {
  snapshot_callback_ = std::move(callback);
}

void WebSocketClient::on_orderbook_delta(DeltaCallback callback) {
  delta_callback_ = std::move(callback);
}

void WebSocketClient::on_fill(FillCallback callback) {
  fill_callback_ = std::move(callback);
}

void WebSocketClient::on_trade(TradeCallback callback) {
  trade_callback_ = std::move(callback);
}

void WebSocketClient::inject_frame(const std::string &raw) {
  handle_message(raw);
}

void WebSocketClient::on_disconnect(DisconnectCallback callback) {
  disconnect_callback_ = std::move(callback);
}

void WebSocketClient::on_reconnect(ReconnectCallback callback) {
  reconnect_callback_ = std::move(callback);
}

std::chrono::steady_clock::time_point
WebSocketClient::last_message_time() const {
  return last_message_time_.load();
}

void WebSocketClient::stop() {
  running_ = false;
  ws_->stop();
}

std::chrono::milliseconds WebSocketClient::next_reconnect_delay() const {
  return reconnect_backoff(reconnect_delay_, consecutive_connect_failures_ + 1);
}

std::map<std::string, std::string> WebSocketClient::auth_headers() const {
  const std::string ws_path = extract_ws_path(ws_url_);
  return auth_.sign("GET", ws_path);
}

void WebSocketClient::send_subscribe(const std::string &ticker) {
  nlohmann::json msg;
  msg["id"] = next_msg_id_++;
  msg["cmd"] = "subscribe";
  msg["params"]["channels"] = {"orderbook_delta", "trade"};
  msg["params"]["market_tickers"] = {ticker};
  ws_->send(msg.dump());
}

void WebSocketClient::send_subscribe_fills() {
  nlohmann::json msg;
  msg["id"] = next_msg_id_++;
  msg["cmd"] = "subscribe";
  msg["params"]["channels"] = {"fill"};
  ws_->send(msg.dump());
}

void WebSocketClient::handle_connect() {
  get_logger()->info("websocket connected url={}", ws_url_);
  consecutive_connect_failures_ = 0;
  last_seq_by_sid_.clear();
  send_subscribe_fills();
  for (const auto &ticker : subscribed_tickers_) {
    send_subscribe(ticker);
  }
  if (has_connected_once_ && reconnect_callback_) {
    reconnect_callback_();
  }
  has_connected_once_ = true;
}

namespace {

// Each dispatcher contains its own parse + callback invocation so that
// handle_message stays a flat type switch. A malformed body is dropped quietly
// (json::exception); a throwing callback is logged but contained — never
// allowed to escape onto the WebSocket thread (which would call
// std::terminate).

void dispatch_snapshot(const WebSocketClient::SnapshotCallback &callback,
                       const nlohmann::json &msg_body) {
  if (!callback) {
    return;
  }
  try {
    callback(parse_snapshot(msg_body));
  } catch (const nlohmann::json::exception &ex) {
    get_logger()->debug("ws dropped malformed snapshot: {}", ex.what());
  } catch (const std::exception &ex) {
    get_logger()->error("ws snapshot callback threw: {}", ex.what());
  }
}

void dispatch_delta(const WebSocketClient::DeltaCallback &callback,
                    const nlohmann::json &msg_body) {
  if (!callback) {
    return;
  }
  try {
    const std::string ticker = msg_body.at("market_ticker").get<std::string>();
    const Side side = parse_side(msg_body.at("side").get<std::string>());
    const int price =
        dollars_to_cents(msg_body.at("price_dollars").get<std::string>());
    const Quantity qty =
        parse_fp_count(msg_body.at("delta_fp").get<std::string>());
    callback(ticker, side, price, qty);
  } catch (const nlohmann::json::exception &ex) {
    get_logger()->debug("ws dropped malformed delta: {}", ex.what());
  } catch (const std::exception &ex) {
    get_logger()->error("ws delta callback threw: {}", ex.what());
  }
}

void dispatch_fill(const WebSocketClient::FillCallback &callback,
                   const nlohmann::json &msg_body) {
  if (!callback) {
    return;
  }
  try {
    Fill fill;
    fill.trade_id = msg_body.value("trade_id", std::string{});
    fill.order_id = msg_body.at("order_id").get<std::string>();
    fill.market_ticker = msg_body.at("market_ticker").get<std::string>();
    fill.side = parse_side(msg_body.at("outcome_side").get<std::string>());
    const int yes_price_cents =
        dollars_to_cents(msg_body.at("yes_price_dollars").get<std::string>());
    fill.price_cents = (fill.side == Side::Yes)
                           ? yes_price_cents
                           : complement_price(yes_price_cents);
    fill.quantity = parse_fp_count(msg_body.at("count_fp").get<std::string>());
    fill.fee_cents = std::stod(msg_body.value("fee_cost", std::string{"0"})) *
                     kCentsPerDollar;
    fill.is_taker = msg_body.at("is_taker").get<bool>();
    const auto ts_ms = msg_body.at("ts_ms").get<long long>();
    fill.timestamp =
        std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms));
    callback(fill);
  } catch (const nlohmann::json::exception &ex) {
    get_logger()->debug("ws dropped malformed fill: {}", ex.what());
  } catch (const std::exception &ex) {
    get_logger()->error("ws fill callback threw: {}", ex.what());
  }
}

void dispatch_trade(const WebSocketClient::TradeCallback &callback,
                    const nlohmann::json &msg_body) {
  if (!callback) {
    return;
  }
  try {
    PublicTrade trade;
    trade.trade_id = msg_body.at("trade_id").get<std::string>();
    trade.market_ticker = msg_body.at("market_ticker").get<std::string>();
    trade.yes_price_cents =
        dollars_to_cents(msg_body.at("yes_price_dollars").get<std::string>());
    trade.quantity = parse_fp_count(msg_body.at("count_fp").get<std::string>());
    trade.taker_side =
        parse_side(msg_body.at("taker_outcome_side").get<std::string>());
    const auto ts_ms = msg_body.at("ts_ms").get<long long>();
    trade.timestamp =
        std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms));
    callback(trade);
  } catch (const nlohmann::json::exception &ex) {
    get_logger()->debug("ws dropped malformed trade: {}", ex.what());
  } catch (const std::exception &ex) {
    get_logger()->error("ws trade callback threw: {}", ex.what());
  }
}

} // namespace

void WebSocketClient::handle_heartbeat() {
  last_message_time_.store(std::chrono::steady_clock::now());
}

bool WebSocketClient::sequence_intact(long long sid, long long seq) {
  const auto [seq_it, first_message] = last_seq_by_sid_.try_emplace(sid, seq);
  if (first_message) {
    return true;
  }
  if (seq == seq_it->second + 1) {
    seq_it->second = seq;
    return true;
  }
  get_logger()->critical(
      "ws seq gap sid={} expected={} got={} — book untrusted, forcing "
      "reconnect to resync",
      sid, seq_it->second + 1, seq);
  last_seq_by_sid_.clear();
  ws_->request_close();
  return false;
}

void WebSocketClient::handle_message(const std::string &raw) {
  last_message_time_.store(std::chrono::steady_clock::now());
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(raw);
  } catch (const nlohmann::json::exception &) {
    return; // Ignore malformed messages.
  }

  const auto type_it = parsed.find("type");
  if (type_it == parsed.end()) {
    return;
  }
  const std::string &msg_type = type_it->get<std::string>();

  if (msg_type == "error") {
    get_logger()->error("ws server error response: {}", raw);
    return;
  }

  const auto msg_it = parsed.find("msg");
  if (msg_it == parsed.end()) {
    return;
  }
  const auto &msg_body = *msg_it;

  if (msg_type == "subscribed") {
    get_logger()->debug("ws subscription confirmed: {}", raw);
    return;
  }

  const auto sid_it = parsed.find("sid");
  const auto seq_it = parsed.find("seq");
  if (sid_it != parsed.end() && seq_it != parsed.end() &&
      sid_it->is_number_integer() && seq_it->is_number_integer() &&
      !sequence_intact(sid_it->get<long long>(), seq_it->get<long long>())) {
    return;
  }

  if (msg_type == "orderbook_snapshot") {
    dispatch_snapshot(snapshot_callback_, msg_body);
  } else if (msg_type == "orderbook_delta") {
    dispatch_delta(delta_callback_, msg_body);
  } else if (msg_type == "fill") {
    dispatch_fill(fill_callback_, msg_body);
  } else if (msg_type == "trade") {
    dispatch_trade(trade_callback_, msg_body);
  }
  // All other message types are silently ignored.
}

void WebSocketClient::run() {
  running_ = true;
  int reconnect_attempts = 0;

  ws_->on_connect([this]() { handle_connect(); });
  ws_->on_message([this](const std::string &raw) { handle_message(raw); });
  ws_->on_heartbeat([this]() { handle_heartbeat(); });
  ws_->on_disconnect([this]() {
    get_logger()->warn("websocket disconnected url={}", ws_url_);
    if (disconnect_callback_) {
      disconnect_callback_();
    }
  });

  while (running_) {
    try {
      ws_->connect(ws_url_, auth_headers());
      ws_->run();
    } catch (const std::exception &ex) {
      get_logger()->critical(
          "ws transport threw ({}); treating as failed attempt", ex.what());
    }

    if (!running_) {
      break;
    }

    if (max_reconnects_ >= 0 && reconnect_attempts >= max_reconnects_) {
      break;
    }
    ++reconnect_attempts;
    ++consecutive_connect_failures_;

    const auto delay =
        reconnect_backoff(reconnect_delay_, consecutive_connect_failures_);
    if (delay.count() > 0) {
      get_logger()->info("ws reconnecting in {}ms (attempt {})", delay.count(),
                         reconnect_attempts);
      std::this_thread::sleep_for(delay);
    }
  }

  running_ = false;
}

} // namespace kalshi
