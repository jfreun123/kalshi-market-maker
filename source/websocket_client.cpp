#include "websocket_client.hpp"
#include "auth.hpp"
#include "logger.hpp"
#include "types.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <ctime>
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
  std::mutex run_mtx;
  std::condition_variable run_cv;
  bool done{false};
};

IxWebSocket::IxWebSocket() : impl_{std::make_unique<Impl>()} {
  impl_->ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
    switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      if (impl_->on_connect_cb) {
        impl_->on_connect_cb();
      }
      break;
    case ix::WebSocketMessageType::Close:
    case ix::WebSocketMessageType::Error:
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
    default:
      break;
    }
  });
}

IxWebSocket::~IxWebSocket() = default;

void IxWebSocket::connect(std::string_view url,
                          const std::map<std::string, std::string> &headers) {
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

Side parse_side(const std::string &side_str) {
  if (side_str == "yes") {
    return Side::Yes;
  }
  return Side::No;
}

// NOLINTNEXTLINE(concurrency-mt-unsafe)
std::chrono::system_clock::time_point parse_iso8601(const std::string &str) {
  std::tm time_fields{};
  // NOLINTNEXTLINE(modernize-use-nullptr)
  if (strptime(str.c_str(), "%Y-%m-%dT%H:%M:%SZ", &time_fields) == nullptr) {
    return std::chrono::system_clock::time_point{};
  }
  return std::chrono::system_clock::from_time_t(timegm(&time_fields));
}

Orderbook parse_snapshot(const nlohmann::json &msg) {
  Orderbook book;
  book.ticker = msg.at("market_ticker").get<std::string>();

  for (const auto &entry : msg.at("yes")) {
    book.yes.push_back({entry.at(0).get<int>(), entry.at(1).get<int>()});
  }
  for (const auto &entry : msg.at("no")) {
    book.no.push_back({entry.at(0).get<int>(), entry.at(1).get<int>()});
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

void WebSocketClient::stop() {
  running_ = false;
  ws_->stop();
}

std::map<std::string, std::string> WebSocketClient::auth_headers() const {
  const std::string ws_path = extract_ws_path(ws_url_);
  return auth_.sign("GET", ws_path);
}

void WebSocketClient::send_subscribe(const std::string &ticker) {
  nlohmann::json msg;
  msg["id"] = next_msg_id_++;
  msg["cmd"] = "subscribe";
  msg["params"]["channels"] = {"orderbook_delta"};
  msg["params"]["market_tickers"] = {ticker};
  ws_->send(msg.dump());
}

void WebSocketClient::handle_connect() {
  get_logger()->info("websocket connected url={}", ws_url_);
  for (const auto &ticker : subscribed_tickers_) {
    send_subscribe(ticker);
  }
}

void WebSocketClient::handle_message(const std::string &raw) {
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
  const auto msg_it = parsed.find("msg");
  if (msg_it == parsed.end()) {
    return;
  }
  const auto &msg_body = *msg_it;

  if (msg_type == "orderbook_snapshot") {
    if (snapshot_callback_) {
      try {
        snapshot_callback_(parse_snapshot(msg_body));
      } catch (const nlohmann::json::exception &) {
        return; // Malformed snapshot; drop message.
      }
    }
  } else if (msg_type == "orderbook_delta") {
    if (delta_callback_) {
      try {
        const std::string ticker =
            msg_body.at("market_ticker").get<std::string>();
        const Side side = parse_side(msg_body.at("side").get<std::string>());
        const int price = msg_body.at("price").get<int>();
        const int qty = msg_body.at("delta").get<int>();
        delta_callback_(ticker, side, price, qty);
      } catch (const nlohmann::json::exception &) {
        return; // Malformed delta; drop message.
      }
    }
  } else if (msg_type == "fill") {
    if (fill_callback_) {
      try {
        Fill fill;
        fill.order_id = msg_body.at("order_id").get<std::string>();
        fill.market_ticker = msg_body.at("market_ticker").get<std::string>();
        fill.side = parse_side(msg_body.at("side").get<std::string>());
        fill.price_cents = msg_body.at("yes_price").get<int>();
        fill.quantity = msg_body.at("count").get<int>();
        fill.timestamp =
            parse_iso8601(msg_body.at("created_time").get<std::string>());
        fill_callback_(fill);
      } catch (const nlohmann::json::exception &) {
        return; // Malformed fill; drop message.
      }
    }
  }
  // All other message types are silently ignored.
}

void WebSocketClient::run() {
  running_ = true;
  int reconnect_attempts = 0;

  ws_->on_connect([this]() { handle_connect(); });
  ws_->on_message([this](const std::string &raw) { handle_message(raw); });
  ws_->on_disconnect([this]() {
    get_logger()->warn("websocket disconnected url={}", ws_url_);
  });

  while (running_) {
    ws_->connect(ws_url_, auth_headers());
    ws_->run();

    if (!running_) {
      break;
    }

    if (max_reconnects_ >= 0 && reconnect_attempts >= max_reconnects_) {
      break;
    }
    ++reconnect_attempts;

    if (reconnect_delay_.count() > 0) {
      std::this_thread::sleep_for(reconnect_delay_);
    }
  }

  running_ = false;
}

} // namespace kalshi
