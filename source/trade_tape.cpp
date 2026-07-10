#include "trade_tape.hpp"

#include <algorithm>
#include <cmath>

namespace kalshi {

TradeTape::TradeTape(TradeTapeConfig config) : config_{config} {}

void TradeTape::record_trade(const PublicTrade &trade) {
  auto &prints = prints_by_ticker_[trade.market_ticker];
  prints.push_back(trade);
  const auto cutoff =
      trade.timestamp - std::chrono::seconds{config_.window_seconds};
  while (!prints.empty() && prints.front().timestamp < cutoff) {
    prints.pop_front();
  }
}

void TradeTape::record_own_fill(const std::string &trade_id) {
  own_trade_ids_.insert(trade_id);
}

bool TradeTape::is_countable(const PublicTrade &print, TimePoint now) const {
  const auto cutoff = now - std::chrono::seconds{config_.window_seconds};
  return print.timestamp >= cutoff && print.timestamp <= now &&
         !own_trade_ids_.contains(print.trade_id);
}

std::optional<double> TradeTape::vwap_cents(std::string_view ticker,
                                            std::chrono::seconds half_life,
                                            TimePoint now) const {
  const auto tape_it = prints_by_ticker_.find(std::string{ticker});
  if (tape_it == prints_by_ticker_.end()) {
    return std::nullopt;
  }
  double weighted_price_sum = 0.0;
  double weight_sum = 0.0;
  for (const auto &print : tape_it->second) {
    if (!is_countable(print, now)) {
      continue;
    }
    const double age_seconds =
        std::chrono::duration<double>(now - print.timestamp).count();
    const double half_lives_elapsed =
        age_seconds / static_cast<double>(half_life.count());
    const double weight =
        print.quantity.contracts() * std::exp2(-half_lives_elapsed);
    weighted_price_sum += weight * print.yes_price_cents;
    weight_sum += weight;
  }
  if (weight_sum <= 0.0) {
    return std::nullopt;
  }
  return weighted_price_sum / weight_sum;
}

int TradeTape::print_count(std::string_view ticker, TimePoint now) const {
  const auto tape_it = prints_by_ticker_.find(std::string{ticker});
  if (tape_it == prints_by_ticker_.end()) {
    return 0;
  }
  int count = 0;
  for (const auto &print : tape_it->second) {
    if (is_countable(print, now)) {
      ++count;
    }
  }
  return count;
}

std::optional<double> TradeTape::minority_side_ratio(std::string_view ticker,
                                                     TimePoint now) const {
  const auto tape_it = prints_by_ticker_.find(std::string{ticker});
  if (tape_it == prints_by_ticker_.end()) {
    return std::nullopt;
  }
  double yes_volume = 0.0;
  double no_volume = 0.0;
  for (const auto &print : tape_it->second) {
    if (!is_countable(print, now)) {
      continue;
    }
    if (print.taker_side == Side::Yes) {
      yes_volume += print.quantity.contracts();
    } else {
      no_volume += print.quantity.contracts();
    }
  }
  const double total_volume = yes_volume + no_volume;
  if (total_volume <= 0.0) {
    return std::nullopt;
  }
  return std::min(yes_volume, no_volume) / total_volume;
}

} // namespace kalshi
