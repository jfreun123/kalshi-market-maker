#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace kalshi {

struct HttpResponse {
  int status_code{200};
  std::string body;
};

class IHttpTransport {
public:
  virtual ~IHttpTransport() = default;

  [[nodiscard]] virtual HttpResponse
  get(std::string_view url,
      const std::map<std::string, std::string> &headers) = 0;

  [[nodiscard]] virtual HttpResponse
  post(std::string_view url, const std::map<std::string, std::string> &headers,
       std::string_view body) = 0;

  [[nodiscard]] virtual HttpResponse
  delete_(std::string_view url,
          const std::map<std::string, std::string> &headers) = 0;
};

// Production HTTP transport backed by cpp-httplib with OpenSSL HTTPS support.
class HttpTransport : public IHttpTransport {
public:
  HttpTransport();
  ~HttpTransport() override;

  HttpTransport(const HttpTransport &) = delete;
  HttpTransport &operator=(const HttpTransport &) = delete;
  HttpTransport(HttpTransport &&) = delete;
  HttpTransport &operator=(HttpTransport &&) = delete;

  [[nodiscard]] HttpResponse
  get(std::string_view url,
      const std::map<std::string, std::string> &headers) override;

  [[nodiscard]] HttpResponse
  post(std::string_view url, const std::map<std::string, std::string> &headers,
       std::string_view body) override;

  [[nodiscard]] HttpResponse
  delete_(std::string_view url,
          const std::map<std::string, std::string> &headers) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kalshi
