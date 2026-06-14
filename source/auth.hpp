#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace kalshi {

// Signs Kalshi REST API requests using RSA-SHA256.
//
// Every request requires three headers:
//   Kalshi-Access-Key       — the API key
//   Kalshi-Access-Timestamp — unix timestamp in milliseconds
//   Kalshi-Access-Signature — base64(RSA_SHA256(timestamp + method + path))
class Auth {
public:
  Auth(std::string api_key, std::string pem_private_key);

  // Returns the three signed headers for a request.
  // timestamp_ms: inject a fixed value in tests for determinism; defaults to
  // now.
  [[nodiscard]] std::map<std::string, std::string>
  sign(std::string_view method, std::string_view path,
       std::optional<long long> timestamp_ms = std::nullopt) const;

private:
  std::string api_key_;
  std::string pem_private_key_;
};

} // namespace kalshi
