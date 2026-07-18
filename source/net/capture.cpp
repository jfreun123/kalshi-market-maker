#include "net/capture.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace kalshi {

CapturingWebSocket::CapturingWebSocket(std::unique_ptr<IWebSocket> inner,
                                       std::ostream &out)
    : inner_{std::move(inner)}, out_{out} {}

void CapturingWebSocket::connect(
    std::string_view url, const std::map<std::string, std::string> &headers) {
  inner_->connect(url, headers);
}

void CapturingWebSocket::send(const std::string &message) {
  inner_->send(message);
}

void CapturingWebSocket::on_message(MessageHandler handler) {
  user_handler_ = std::move(handler);
  inner_->on_message([this](const std::string &message) {
    out_ << message << '\n';
    out_.flush(); // survive an abrupt Ctrl-C stop
    captured_count_.fetch_add(1);
    if (user_handler_) {
      user_handler_(message);
    }
  });
}

void CapturingWebSocket::on_connect(ConnectHandler handler) {
  inner_->on_connect(std::move(handler));
}

void CapturingWebSocket::on_disconnect(DisconnectHandler handler) {
  inner_->on_disconnect(std::move(handler));
}

void CapturingWebSocket::on_heartbeat(HeartbeatHandler handler) {
  inner_->on_heartbeat(std::move(handler));
}

void CapturingWebSocket::run() { inner_->run(); }

void CapturingWebSocket::request_close() { inner_->request_close(); }

void CapturingWebSocket::stop() { inner_->stop(); }

CapturingHttpTransport::CapturingHttpTransport(
    std::unique_ptr<IHttpTransport> inner, std::ostream &out)
    : inner_{std::move(inner)}, out_{out} {}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — internal recorder
HttpResponse CapturingHttpTransport::record(std::string_view method,
                                            std::string_view url,
                                            std::string_view request_body,
                                            HttpResponse response) {
  nlohmann::json record_json;
  record_json["method"] = method;
  record_json["url"] = url;
  record_json["request_body"] = request_body;
  record_json["status"] = response.status_code;
  record_json["response_body"] = response.body;
  out_ << record_json.dump() << '\n';
  out_.flush();
  captured_count_.fetch_add(1);
  return response;
}

HttpResponse
CapturingHttpTransport::get(std::string_view url,
                            const std::map<std::string, std::string> &headers) {
  return record("GET", url, "", inner_->get(url, headers));
}

HttpResponse
CapturingHttpTransport::post(std::string_view url,
                             const std::map<std::string, std::string> &headers,
                             std::string_view body) {
  return record("POST", url, body, inner_->post(url, headers, body));
}

HttpResponse CapturingHttpTransport::delete_(
    std::string_view url, const std::map<std::string, std::string> &headers) {
  return record("DELETE", url, "", inner_->delete_(url, headers));
}

} // namespace kalshi
