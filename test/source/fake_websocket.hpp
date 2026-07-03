#pragma once

#include "websocket_client.hpp"

#include <map>
#include <string>
#include <vector>

namespace kalshi {

// Test double for IWebSocket.
//
// Delivers queued messages synchronously when run() is called:
//   1. Fires the on_connect handler.
//   2. Delivers each enqueued message via the on_message handler.
//   3. If trigger_disconnect() was called, fires the on_disconnect handler.
// Returns immediately after step 3. No threads.
//
// On reconnect (second call to connect()), the same behavior repeats but
// with a fresh message queue so tests can control each reconnect independently.

class FakeWebSocket : public IWebSocket {
public:
  void enqueue_message(std::string message) {
    messages_.push_back(std::move(message));
  }

  // Cause the on_disconnect handler to fire at the end of the next run().
  void trigger_disconnect() { fire_disconnect_ = true; }

  // Cause the on_heartbeat handler to fire at the start of the next run().
  void trigger_heartbeat() { fire_heartbeat_ = true; }

  // Inspection
  [[nodiscard]] int connect_count() const { return connect_count_; }
  [[nodiscard]] const std::string &connected_url() const {
    return connected_url_;
  }
  [[nodiscard]] const std::vector<std::string> &sent_messages() const {
    return sent_messages_;
  }
  [[nodiscard]] const std::map<std::string, std::string> &
  connected_headers() const {
    return connected_headers_;
  }

  // ---- IWebSocket ----

  void connect(std::string_view url,
               const std::map<std::string, std::string> &headers) override {
    ++connect_count_;
    connected_url_ = url;
    connected_headers_ = headers;
  }

  void send(const std::string &message) override {
    sent_messages_.push_back(message);
  }

  void on_message(MessageHandler handler) override {
    msg_handler_ = std::move(handler);
  }

  void on_connect(ConnectHandler handler) override {
    connect_handler_ = std::move(handler);
  }

  void on_disconnect(DisconnectHandler handler) override {
    disconnect_handler_ = std::move(handler);
  }

  void on_heartbeat(HeartbeatHandler handler) override {
    heartbeat_handler_ = std::move(handler);
  }

  void run() override {
    if (connect_handler_) {
      connect_handler_();
    }
    if (fire_heartbeat_) {
      fire_heartbeat_ = false;
      if (heartbeat_handler_) {
        heartbeat_handler_();
      }
    }
    for (const auto &msg : messages_) {
      if (msg_handler_) {
        msg_handler_(msg);
      }
    }
    messages_.clear();
    if (fire_disconnect_) {
      fire_disconnect_ = false;
      if (disconnect_handler_) {
        disconnect_handler_();
      }
    }
  }

  void stop() override { stopped_ = true; }

  [[nodiscard]] bool was_stopped() const { return stopped_; }

private:
  std::vector<std::string> messages_;
  std::vector<std::string> sent_messages_;
  std::string connected_url_;
  std::map<std::string, std::string> connected_headers_;
  int connect_count_{0};
  bool fire_disconnect_{false};
  bool fire_heartbeat_{false};
  bool stopped_{false};

  MessageHandler msg_handler_;
  ConnectHandler connect_handler_;
  DisconnectHandler disconnect_handler_;
  HeartbeatHandler heartbeat_handler_;
};

} // namespace kalshi
