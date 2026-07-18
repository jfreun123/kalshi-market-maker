#pragma once
// Structured JSONL analytics for offline strategy measurement (PLAN item 31):
// one line per quote decision and per fill, written through an injectable sink
// (production: a dedicated spdlog file) so markout, effective spread, and
// calibration are computed offline from machine-readable events instead of
// the human log. Every fill should answer: good fill, or adverse selection?

#include "core/types.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace kalshi {

struct QuoteDecision {
  std::string_view ticker;
  double mid_cents{0.0};
  double micro_cents{0.0};
  double fair_value_cents{0.0};
  int bid_cents{0};
  int ask_cents{0};
  double inventory_contracts{0.0};
  bool flow_imbalanced{false};
};

class AnalyticsLogger {
public:
  using Sink = std::function<void(const std::string &)>;
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  explicit AnalyticsLogger(Sink sink, Clock clock = {});

  void quote_decision(const QuoteDecision &decision);
  void fill(const Fill &fill_event, double mid_cents,
            double inventory_after_contracts);
  void http_latency(std::string_view method, std::string_view path,
                    int status_code, long long rtt_ms);

private:
  Sink sink_;
  Clock clock_;
};

} // namespace kalshi
