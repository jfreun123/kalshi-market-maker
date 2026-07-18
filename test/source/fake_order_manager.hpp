#pragma once
// Minimal IOrderManager fake shared by test suites: returns values from
// settable per-ticker maps so read-model and session logic can be tested in
// isolation. A struct (passive data holder) so its maps are intentionally
// public for direct test setup.

#include "order_manager_iface.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

struct FakeOrderManager : public kalshi::IOrderManager {
  std::unordered_map<std::string, kalshi::Quantity> positions;
  std::unordered_map<std::string, double> realized;
  std::unordered_map<std::string, double> fees;
  std::unordered_map<std::string, double> unrealized;
  std::unordered_map<std::string, double> cost;
  std::unordered_map<std::string, kalshi::Order> open_map;

  kalshi::Order place(std::string_view /*ticker*/, kalshi::Side /*side*/,
                      int /*price_cents*/, int /*quantity*/) override {
    return {};
  }
  bool cancel(std::string_view /*order_id*/) override { return true; }
  void cancel_all(std::string_view /*ticker*/) override {}
  std::optional<std::string> amend(std::string_view order_id, std::string_view,
                                   kalshi::Side, int,
                                   kalshi::Quantity) override {
    return std::string{order_id};
  }
  bool record_fill(const kalshi::Fill & /*fill*/) override { return true; }

  [[nodiscard]] kalshi::Quantity
  net_position(std::string_view ticker) const override {
    auto iter = positions.find(std::string{ticker});
    return iter == positions.end() ? kalshi::Quantity{} : iter->second;
  }
  [[nodiscard]] double realized_pnl(std::string_view ticker) const override {
    auto iter = realized.find(std::string{ticker});
    return iter == realized.end() ? 0.0 : iter->second;
  }
  [[nodiscard]] double fees_paid(std::string_view ticker) const override {
    auto iter = fees.find(std::string{ticker});
    return iter == fees.end() ? 0.0 : iter->second;
  }
  [[nodiscard]] double unrealized_pnl(std::string_view ticker,
                                      int /*yes_mid_cents*/) const override {
    auto iter = unrealized.find(std::string{ticker});
    return iter == unrealized.end() ? 0.0 : iter->second;
  }
  [[nodiscard]] double position_cost(std::string_view ticker) const override {
    auto iter = cost.find(std::string{ticker});
    return iter == cost.end() ? 0.0 : iter->second;
  }
  [[nodiscard]] kalshi::ExposureDecomposition
  exposure_decomposition(std::string_view /*ticker*/) const override {
    return {};
  }
  [[nodiscard]] const std::unordered_map<std::string, kalshi::Order> &
  open_orders() const override {
    return open_map;
  }
};
