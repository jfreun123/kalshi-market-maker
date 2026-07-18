#include "engine/analytics.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace kalshi {

namespace {

long long epoch_ms(std::chrono::system_clock::time_point when) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             when.time_since_epoch())
      .count();
}

} // namespace

AnalyticsLogger::AnalyticsLogger(Sink sink, Clock clock)
    : sink_{std::move(sink)},
      clock_{clock ? std::move(clock)
                   : Clock{[] { return std::chrono::system_clock::now(); }}} {}

void AnalyticsLogger::quote_decision(const QuoteDecision &decision) {
  if (!sink_) {
    return;
  }
  const nlohmann::json event = {
      {"type", "quote"},
      {"ts_ms", epoch_ms(clock_())},
      {"ticker", decision.ticker},
      {"mid", decision.mid_cents},
      {"micro", decision.micro_cents},
      {"fv", decision.fair_value_cents},
      {"bid", decision.bid_cents},
      {"ask", decision.ask_cents},
      {"inventory", decision.inventory_contracts},
      {"imbalanced", decision.flow_imbalanced},
  };
  sink_(event.dump());
}

void AnalyticsLogger::fill(const Fill &fill_event, double mid_cents,
                           double inventory_after_contracts) {
  if (!sink_) {
    return;
  }
  const nlohmann::json event = {
      {"type", "fill"},
      {"ts_ms", epoch_ms(clock_())},
      {"ticker", fill_event.market_ticker},
      {"trade_id", fill_event.trade_id},
      {"side", fill_event.side == Side::Yes ? "yes" : "no"},
      {"price", fill_event.price_cents},
      {"qty", fill_event.quantity.contracts()},
      {"fee_cents", fill_event.fee_cents},
      {"is_taker", fill_event.is_taker},
      {"mid", mid_cents},
      {"inventory_after", inventory_after_contracts},
  };
  sink_(event.dump());
}

void AnalyticsLogger::http_latency(std::string_view method,
                                   std::string_view path, int status_code,
                                   long long rtt_ms) {
  if (!sink_) {
    return;
  }
  const nlohmann::json event = {
      {"type", "http"}, {"ts_ms", epoch_ms(clock_())}, {"method", method},
      {"path", path},   {"status", status_code},       {"rtt_ms", rtt_ms},
  };
  sink_(event.dump());
}

} // namespace kalshi
