#include "http_transport.hpp"

#include "logger.hpp"

#include <httplib.h>

#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace kalshi {

namespace {

// Converts our header map to httplib's header multimap.
httplib::Headers
to_httplib_headers(const std::map<std::string, std::string> &headers) {
  httplib::Headers result;
  for (const auto &[key, value] : headers) {
    result.emplace(key, value);
  }
  return result;
}

// Parses a full URL to extract the scheme+host base (e.g. "https://host.com")
// and the path component (e.g. "/trade-api/v2/markets").
struct SplitUrl {
  std::string base; // scheme + host (+ optional port)
  std::string path; // path + query string
};

SplitUrl split_url(std::string_view url_view) {
  std::string url{url_view};
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    throw std::runtime_error("Invalid URL (no scheme): " + url);
  }
  auto path_start = url.find('/', scheme_end + 3U);
  if (path_start == std::string::npos) {
    return {url, "/"};
  }
  return {url.substr(0, path_start), url.substr(path_start)};
}

HttpResponse make_response(const httplib::Result &result) {
  if (!result) {
    throw std::runtime_error("HTTP request failed: " +
                             httplib::to_string(result.error()));
  }
  return {result->status, result->body};
}

constexpr int kConnectTimeoutSec = 10;
constexpr int kReadTimeoutSec = 30;

void configure(httplib::Client &client) {
  client.set_follow_location(true);
  client.set_connection_timeout(kConnectTimeoutSec, 0);
  client.set_read_timeout(kReadTimeoutSec, 0);
  client.set_keep_alive(true);
}

template <typename PerformFn>
HttpResponse timed(std::string_view method, const std::string &path,
                   const HttpTransport::LatencyObserver &observer,
                   PerformFn &&perform) {
  const auto start = std::chrono::steady_clock::now();
  httplib::Result result = std::forward<PerformFn>(perform)();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  const int status = result ? result->status : -1;
  get_logger()->debug("http {} {} status={} rtt={}ms", method, path, status,
                      elapsed_ms);
  if (observer) {
    observer(method, path, status, elapsed_ms);
  }
  return make_response(result);
}

} // namespace

struct HttpTransport::Impl {
  std::mutex mutex;
  std::map<std::string, httplib::Client> clients;
  LatencyObserver latency_observer;

  httplib::Client &client_for(const std::string &base) {
    auto iter = clients.find(base);
    if (iter == clients.end()) {
      iter = clients.try_emplace(base, base).first;
      configure(iter->second);
    }
    return iter->second;
  }
};

HttpTransport::HttpTransport() : impl_{std::make_unique<Impl>()} {}

void HttpTransport::set_latency_observer(LatencyObserver observer) {
  impl_->latency_observer = std::move(observer);
}
HttpTransport::~HttpTransport() = default;

HttpResponse
HttpTransport::get(std::string_view url,
                   const std::map<std::string, std::string> &headers) {
  auto [base, path] = split_url(url);
  auto httplib_headers = to_httplib_headers(headers);
  const std::lock_guard<std::mutex> lock{impl_->mutex};
  httplib::Client &client = impl_->client_for(base);
  return timed("GET", path, impl_->latency_observer,
               [&] { return client.Get(path, httplib_headers); });
}

HttpResponse
HttpTransport::post(std::string_view url,
                    const std::map<std::string, std::string> &headers,
                    std::string_view body) {
  auto [base, path] = split_url(url);
  auto httplib_headers = to_httplib_headers(headers);
  const std::lock_guard<std::mutex> lock{impl_->mutex};
  httplib::Client &client = impl_->client_for(base);
  return timed("POST", path, impl_->latency_observer, [&] {
    return client.Post(path, httplib_headers, std::string(body),
                       "application/json");
  });
}

HttpResponse
HttpTransport::delete_(std::string_view url,
                       const std::map<std::string, std::string> &headers) {
  auto [base, path] = split_url(url);
  auto httplib_headers = to_httplib_headers(headers);
  const std::lock_guard<std::mutex> lock{impl_->mutex};
  httplib::Client &client = impl_->client_for(base);
  return timed("DELETE", path, impl_->latency_observer,
               [&] { return client.Delete(path, httplib_headers); });
}

} // namespace kalshi
