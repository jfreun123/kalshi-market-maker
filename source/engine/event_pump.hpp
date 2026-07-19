#pragma once
// Hands market events from the WebSocket thread to a single consumer thread
// in arrival order (PLAN L3). WS callbacks reduce to a queue push guarded by
// a queue-only mutex, so the socket thread never waits behind an order REST
// call; the consumer drains batches and applies them to the TradingSession
// under the engine lock. Ordering across event kinds is exactly arrival
// order; high_water exposes the deepest backlog seen for observability.

#include "core/types.hpp"
#include "engine/orderbook.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace kalshi {

struct BookDeltaEvent {
  std::string ticker;
  Side side{Side::Yes};
  int price_cents{0};
  Quantity quantity{};
};

struct DisconnectEvent {};

using SessionEvent =
    std::variant<Orderbook, BookDeltaEvent, PublicTrade, Fill, DisconnectEvent>;

class EventPump {
public:
  void push(SessionEvent event);

  // Blocks until events arrive, the timeout lapses, or stop() is called;
  // drained events are appended to sink in arrival order. Returns false only
  // when stopped AND nothing was drained — the consumer's exit signal.
  bool wait_drain(std::vector<SessionEvent> &sink,
                  std::chrono::milliseconds timeout);

  void stop();

  [[nodiscard]] std::size_t high_water() const;

private:
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<SessionEvent> queue_;
  std::size_t high_water_{0U};
  bool stopped_{false};
};

} // namespace kalshi
