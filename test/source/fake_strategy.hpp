#pragma once
// IStrategy fake that records every call, for testing the session-to-strategy
// seam without any quoting logic.

#include "strategy.hpp"

#include <string>
#include <string_view>
#include <vector>

struct FakeStrategy : public kalshi::IStrategy {
  std::vector<std::string> updated_tickers;
  std::vector<std::string> forgotten_orders;
  std::vector<std::string> forgotten_tickers;
  int reset_count{0};
  bool reduce_only{false};

  void update(std::string_view ticker,
              const kalshi::LocalOrderbook & /*book*/) override {
    updated_tickers.emplace_back(ticker);
  }
  void forget_order(std::string_view ticker,
                    std::string_view order_id) override {
    forgotten_orders.push_back(std::string{ticker} + ":" +
                               std::string{order_id});
  }
  void forget_ticker(std::string_view ticker) override {
    forgotten_tickers.emplace_back(ticker);
  }
  void reset_quotes() override { ++reset_count; }
  void set_reduce_only(bool value) override { reduce_only = value; }
};
