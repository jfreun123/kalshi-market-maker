#pragma once

#include "http_transport.hpp"

#include <map>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

// FakeTransport is a test double for IHttpTransport.
// Pre-load responses with enqueue(); inspect outgoing requests via
// recorded_requests().
class FakeTransport : public kalshi::IHttpTransport {
public:
  struct RecordedRequest {
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
  };

  void enqueue(kalshi::HttpResponse response) {
    queued_responses_.push(std::move(response));
  }

  [[nodiscard]] const std::vector<RecordedRequest> &recorded_requests() const {
    return recorded_requests_;
  }

  [[nodiscard]] const RecordedRequest &last_request() const {
    return recorded_requests_.back();
  }

  kalshi::HttpResponse
  get(std::string_view url,
      const std::map<std::string, std::string> &headers) override {
    return consume("GET", url, headers, "");
  }

  kalshi::HttpResponse post(std::string_view url,
                            const std::map<std::string, std::string> &headers,
                            std::string_view body) override {
    return consume("POST", url, headers, std::string(body));
  }

  kalshi::HttpResponse
  delete_(std::string_view url,
          const std::map<std::string, std::string> &headers) override {
    return consume("DELETE", url, headers, "");
  }

private:
  kalshi::HttpResponse
  consume(std::string method, std::string_view url,
          const std::map<std::string, std::string> &headers, std::string body) {
    recorded_requests_.push_back(
        {std::move(method), std::string(url), headers, std::move(body)});
    if (queued_responses_.empty()) {
      return {200, "{}"};
    }
    auto response = std::move(queued_responses_.front());
    queued_responses_.pop();
    return response;
  }

  std::queue<kalshi::HttpResponse> queued_responses_;
  std::vector<RecordedRequest> recorded_requests_;
};
