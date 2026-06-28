#include "http_transport.hpp"

#include <httplib.h>

#include <stdexcept>
#include <string>

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

// Performs a request via a pre-configured httplib::Client and returns response.
HttpResponse make_response(const httplib::Result &result) {
  if (!result) {
    throw std::runtime_error("HTTP request failed: " +
                             httplib::to_string(result.error()));
  }
  return {result->status, result->body};
}

} // namespace

namespace {

constexpr int kConnectTimeoutSec = 10;
constexpr int kReadTimeoutSec = 30;

httplib::Client make_client(const std::string &base) {
  httplib::Client client{base};
  client.set_follow_location(true);
  client.set_connection_timeout(kConnectTimeoutSec, 0);
  client.set_read_timeout(kReadTimeoutSec, 0);
  return client;
}

} // namespace

HttpResponse
HttpTransport::get(std::string_view url,
                   const std::map<std::string, std::string> &headers) {
  auto [base, path] = split_url(url);
  auto client = make_client(base);
  auto httplib_headers = to_httplib_headers(headers);
  return make_response(client.Get(path, httplib_headers));
}

HttpResponse
HttpTransport::post(std::string_view url,
                    const std::map<std::string, std::string> &headers,
                    std::string_view body) {
  auto [base, path] = split_url(url);
  auto client = make_client(base);
  auto httplib_headers = to_httplib_headers(headers);
  return make_response(client.Post(path, httplib_headers, std::string(body),
                                   "application/json"));
}

HttpResponse
HttpTransport::delete_(std::string_view url,
                       const std::map<std::string, std::string> &headers) {
  auto [base, path] = split_url(url);
  auto client = make_client(base);
  auto httplib_headers = to_httplib_headers(headers);
  return make_response(client.Delete(path, httplib_headers));
}

} // namespace kalshi
