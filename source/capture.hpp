#pragma once

#include "http_transport.hpp"
#include "websocket_client.hpp"

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace kalshi {

// IWebSocket decorator that tees every inbound frame to an output stream — one
// raw message per line, directly replay-compatible with the session fixtures —
// before forwarding it to the wrapped client's handler. connect/send/lifecycle
// calls pass straight through to the inner socket.
class CapturingWebSocket : public IWebSocket {
public:
  CapturingWebSocket(std::unique_ptr<IWebSocket> inner, std::ostream &out);

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

  [[nodiscard]] std::size_t captured_count() const {
    return captured_count_.load();
  }

private:
  std::unique_ptr<IWebSocket> inner_;
  std::ostream &out_;
  MessageHandler user_handler_;
  std::atomic<std::size_t> captured_count_{0};
};

// IHttpTransport decorator that records each request/response as one JSON line
// ({method,url,request_body,status,response_body}) to an output stream, then
// returns the wrapped transport's response unchanged. Lets a capture run dump
// the raw REST field shapes (orderbook seeds, positions) for inspection.
class CapturingHttpTransport : public IHttpTransport {
public:
  CapturingHttpTransport(std::unique_ptr<IHttpTransport> inner,
                         std::ostream &out);

  HttpResponse get(std::string_view url,
                   const std::map<std::string, std::string> &headers) override;
  HttpResponse post(std::string_view url,
                    const std::map<std::string, std::string> &headers,
                    std::string_view body) override;
  HttpResponse
  delete_(std::string_view url,
          const std::map<std::string, std::string> &headers) override;

  [[nodiscard]] std::size_t captured_count() const {
    return captured_count_.load();
  }

private:
  HttpResponse record(std::string_view method, std::string_view url,
                      std::string_view request_body, HttpResponse response);

  std::unique_ptr<IHttpTransport> inner_;
  std::ostream &out_;
  std::atomic<std::size_t> captured_count_{0};
};

} // namespace kalshi
