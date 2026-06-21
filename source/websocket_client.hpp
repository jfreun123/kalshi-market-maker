#pragma once

#include "auth.hpp"
#include "types.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kalshi {

// ---- Raw WebSocket transport interface ----
//
// Abstracts the connection so unit tests can inject FakeWebSocket.
// Production code uses IxWebSocket (ixwebsocket library).

class IWebSocket {
public:
  using MessageHandler = std::function<void(const std::string &)>;
  using ConnectHandler = std::function<void()>;
  using DisconnectHandler = std::function<void()>;

  virtual ~IWebSocket() = default;

  // Establish the connection (called before run()).
  virtual void connect(std::string_view url,
                       const std::map<std::string, std::string> &headers) = 0;

  // Send a text frame.
  virtual void send(const std::string &message) = 0;

  // Register event handlers (called before run()).
  virtual void on_message(MessageHandler handler) = 0;
  virtual void on_connect(ConnectHandler handler) = 0;
  virtual void on_disconnect(DisconnectHandler handler) = 0;

  // Block until the connection closes or stop() is called.
  virtual void run() = 0;

  // Signals run() to return.
  virtual void stop() = 0;
};

// Production IWebSocket backed by ixwebsocket.
// Fully implemented in Phase 10 when the main binary is wired up.
class IxWebSocket : public IWebSocket {
public:
  IxWebSocket();
  ~IxWebSocket() override;

  IxWebSocket(const IxWebSocket &) = delete;
  IxWebSocket &operator=(const IxWebSocket &) = delete;
  IxWebSocket(IxWebSocket &&) = delete;
  IxWebSocket &operator=(IxWebSocket &&) = delete;

  void connect(std::string_view url,
               const std::map<std::string, std::string> &headers) override;
  void send(const std::string &message) override;
  void on_message(MessageHandler handler) override;
  void on_connect(ConnectHandler handler) override;
  void on_disconnect(DisconnectHandler handler) override;
  void run() override;
  void stop() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- Kalshi protocol client ----
//
// Wraps IWebSocket and handles:
//   - subscribe JSON formatting
//   - parsing incoming snapshot/delta/fill messages
//   - automatic reconnect with configurable delay

class WebSocketClient {
public:
  using SnapshotCallback = std::function<void(const Orderbook &)>;
  using DeltaCallback =
      std::function<void(const std::string &ticker, Side, int price, int qty)>;
  using FillCallback = std::function<void(const Fill &)>;

  // max_reconnects: number of reconnect attempts after the first disconnect.
  //   -1 = unlimited (production default).
  //    0 = no reconnect (single run).
  // reconnect_delay: wait between reconnects; 0ms is useful in tests.
  explicit WebSocketClient(
      Auth auth, std::unique_ptr<IWebSocket> ws_transport,
      std::string base_url = "wss://external-api-ws.kalshi.com/trade-api/ws/v2",
      int max_reconnects = -1,
      std::chrono::milliseconds reconnect_delay = std::chrono::seconds{5});

  void subscribe(std::string_view ticker);

  void on_orderbook_snapshot(SnapshotCallback callback);
  void on_orderbook_delta(DeltaCallback callback);
  void on_fill(FillCallback callback);

  // Blocks until stop() is called or max_reconnects exhausted.
  void run();
  void stop();

private:
  void send_subscribe(const std::string &ticker);
  void send_subscribe_fills();
  void handle_connect();
  void handle_message(const std::string &raw);

  [[nodiscard]] std::map<std::string, std::string> auth_headers() const;

  Auth auth_;
  std::unique_ptr<IWebSocket> ws_;
  std::string ws_url_;
  int max_reconnects_;
  std::chrono::milliseconds reconnect_delay_;

  bool running_{false};
  int next_msg_id_{1};
  std::vector<std::string> subscribed_tickers_;

  SnapshotCallback snapshot_callback_;
  DeltaCallback delta_callback_;
  FillCallback fill_callback_;
};

} // namespace kalshi
