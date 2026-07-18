#pragma once

#include "auth.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
  using HeartbeatHandler = std::function<void()>;

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
  virtual void on_heartbeat(HeartbeatHandler handler) = 0;

  // Block until the connection closes or stop() is called.
  virtual void run() = 0;

  // Signals run() to return.
  virtual void stop() = 0;

  virtual void request_close() = 0;
};

// No-op transport for replay tooling: lets WebSocketClient parse frames via
// inject_frame without any connection, so offline replays reuse the exact
// production message parsing.
class NullWebSocket : public IWebSocket {
public:
  void
  connect(std::string_view /*url*/,
          const std::map<std::string, std::string> & /*headers*/) override {}
  void send(const std::string & /*message*/) override {}
  void on_message(MessageHandler /*handler*/) override {}
  void on_connect(ConnectHandler /*handler*/) override {}
  void on_disconnect(DisconnectHandler /*handler*/) override {}
  void on_heartbeat(HeartbeatHandler /*handler*/) override {}
  void run() override {}
  void stop() override {}
  void request_close() override {}
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
  void on_heartbeat(HeartbeatHandler handler) override;
  void run() override;
  void stop() override;
  void request_close() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

inline constexpr std::chrono::seconds kMaxReconnectDelay{60};

[[nodiscard]] std::chrono::milliseconds
reconnect_backoff(std::chrono::milliseconds base_delay,
                  int consecutive_failures);

// ---- Kalshi protocol client ----
//
// Wraps IWebSocket and handles:
//   - subscribe JSON formatting
//   - parsing incoming snapshot/delta/fill messages
//   - automatic reconnect with configurable delay

class WebSocketClient {
public:
  using SnapshotCallback = std::function<void(const Orderbook &)>;
  using DeltaCallback = std::function<void(const std::string &ticker, Side,
                                           int price, Quantity delta)>;
  using FillCallback = std::function<void(const Fill &)>;
  using TradeCallback = std::function<void(const PublicTrade &)>;
  using DisconnectCallback = std::function<void()>;
  using ReconnectCallback = std::function<void()>;

  // max_reconnects: number of reconnect attempts after the first disconnect.
  //   -1 = unlimited (production default).
  //    0 = no reconnect (single run).
  // reconnect_delay: base wait between reconnects, doubled per consecutive
  //   failed connection up to kMaxReconnectDelay; 0ms is useful in tests.
  explicit WebSocketClient(
      Auth auth, std::unique_ptr<IWebSocket> ws_transport,
      std::string base_url = "wss://external-api-ws.kalshi.com/trade-api/ws/v2",
      int max_reconnects = -1,
      std::chrono::milliseconds reconnect_delay = std::chrono::seconds{5});

  void subscribe(std::string_view ticker);

  void on_orderbook_snapshot(SnapshotCallback callback);
  void on_orderbook_delta(DeltaCallback callback);
  void on_fill(FillCallback callback);
  void on_trade(TradeCallback callback);
  void on_disconnect(DisconnectCallback callback);

  // Fires after a connection is re-established and subscriptions are resent —
  // never on the first connect. Fills that arrived while disconnected are only
  // ever pushed once, so this is the hook for a REST fill backfill.
  void on_reconnect(ReconnectCallback callback);

  // Time of the last message received. Initialized to construction time.
  // Use this to detect a silently stalled connection.
  [[nodiscard]] std::chrono::steady_clock::time_point last_message_time() const;

  // Feed one raw frame through the production parse/dispatch path without a
  // connection — the seam replay tooling uses to grade recorded sessions.
  void inject_frame(const std::string &raw);

  // Blocks until stop() is called or max_reconnects exhausted.
  void run();
  void stop();

  [[nodiscard]] std::chrono::milliseconds next_reconnect_delay() const;

private:
  void send_subscribe(const std::string &ticker);
  void send_subscribe_fills();
  void handle_connect();
  void handle_message(const std::string &raw);
  void handle_heartbeat();
  [[nodiscard]] bool sequence_intact(long long sid, long long seq);

  [[nodiscard]] std::map<std::string, std::string> auth_headers() const;

  Auth auth_;
  std::unique_ptr<IWebSocket> ws_;
  std::string ws_url_;
  int max_reconnects_;
  std::chrono::milliseconds reconnect_delay_;

  bool running_{false};
  int next_msg_id_{1};
  int consecutive_connect_failures_{0};
  bool has_connected_once_{false};
  // Guards subscribed_tickers_ and connected_: subscribe() is called from the
  // main loop (rotation) while handle_connect/disconnect run on the WS thread.
  std::mutex subscribe_mtx_;
  bool connected_{false};
  std::vector<std::string> subscribed_tickers_;
  std::map<long long, long long> last_seq_by_sid_;

  SnapshotCallback snapshot_callback_;
  DeltaCallback delta_callback_;
  FillCallback fill_callback_;
  TradeCallback trade_callback_;
  DisconnectCallback disconnect_callback_;
  ReconnectCallback reconnect_callback_;

  std::atomic<std::chrono::steady_clock::time_point> last_message_time_{
      std::chrono::steady_clock::now()};
};

} // namespace kalshi
