#pragma once

// A contract count on Kalshi, stored as an exact integer number of
// centi-contracts (hundredths of a contract). Kalshi reports counts as
// fixed-point strings with up to two decimals and fills can be fractional, so
// storing whole ints dropped sub-unit orderbook deltas and fractional fills.
// Centi-contract integers keep the 0.01 granularity exactly, with no
// floating-point drift. A Quantity is signed: positions net YES minus NO.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>

namespace kalshi {

class Quantity {
public:
  static constexpr std::int64_t kCentiPerContract = 100;

  Quantity() = default;

  [[nodiscard]] static Quantity from_centi(std::int64_t centi) {
    return Quantity{centi};
  }

  [[nodiscard]] static Quantity from_contracts(std::int64_t contracts) {
    return Quantity{contracts * kCentiPerContract};
  }

  [[nodiscard]] static Quantity from_fp_string(const std::string &fixed_point) {
    return Quantity{static_cast<std::int64_t>(std::llround(
        std::stod(fixed_point) * static_cast<double>(kCentiPerContract)))};
  }

  [[nodiscard]] std::int64_t centi() const { return centi_; }

  [[nodiscard]] double contracts() const {
    return static_cast<double>(centi_) / static_cast<double>(kCentiPerContract);
  }

  [[nodiscard]] std::string to_fp_string() const {
    const char *sign = (centi_ < 0) ? "-" : "";
    const std::int64_t magnitude = std::abs(centi_);
    return std::format("{}{}.{:02d}", sign, magnitude / kCentiPerContract,
                       magnitude % kCentiPerContract);
  }

  [[nodiscard]] bool is_zero() const { return centi_ == 0; }
  [[nodiscard]] bool is_positive() const { return centi_ > 0; }

  Quantity operator-() const { return Quantity{-centi_}; }

  Quantity &operator+=(Quantity other) {
    centi_ += other.centi_;
    return *this;
  }

  Quantity &operator-=(Quantity other) {
    centi_ -= other.centi_;
    return *this;
  }

  friend Quantity operator+(Quantity lhs, Quantity rhs) { return lhs += rhs; }
  friend Quantity operator-(Quantity lhs, Quantity rhs) { return lhs -= rhs; }

  friend bool operator==(Quantity lhs, Quantity rhs) {
    return lhs.centi_ == rhs.centi_;
  }
  friend bool operator!=(Quantity lhs, Quantity rhs) {
    return lhs.centi_ != rhs.centi_;
  }
  friend bool operator<(Quantity lhs, Quantity rhs) {
    return lhs.centi_ < rhs.centi_;
  }
  friend bool operator>(Quantity lhs, Quantity rhs) {
    return lhs.centi_ > rhs.centi_;
  }
  friend bool operator<=(Quantity lhs, Quantity rhs) {
    return lhs.centi_ <= rhs.centi_;
  }
  friend bool operator>=(Quantity lhs, Quantity rhs) {
    return lhs.centi_ >= rhs.centi_;
  }

private:
  explicit Quantity(std::int64_t centi) : centi_{centi} {}

  std::int64_t centi_{0};
};

[[nodiscard]] inline Quantity abs(Quantity value) {
  return value.is_positive() || value.is_zero() ? value : -value;
}

[[nodiscard]] inline Quantity min(Quantity lhs, Quantity rhs) {
  return (lhs < rhs) ? lhs : rhs;
}

} // namespace kalshi
