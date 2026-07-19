#include "engine/event_pump.hpp"

#include <algorithm>

namespace kalshi {

void EventPump::push(SessionEvent event) {
  {
    const std::lock_guard<std::mutex> lock{mtx_};
    queue_.push_back(std::move(event));
    high_water_ = std::max(high_water_, queue_.size());
  }
  cv_.notify_one();
}

bool EventPump::wait_drain(std::vector<SessionEvent> &sink,
                           std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock{mtx_};
  cv_.wait_for(lock, timeout, [this]() { return !queue_.empty() || stopped_; });
  const bool drained_any = !queue_.empty();
  while (!queue_.empty()) {
    sink.push_back(std::move(queue_.front()));
    queue_.pop_front();
  }
  return drained_any || !stopped_;
}

void EventPump::stop() {
  {
    const std::lock_guard<std::mutex> lock{mtx_};
    stopped_ = true;
  }
  cv_.notify_all();
}

std::size_t EventPump::high_water() const {
  const std::lock_guard<std::mutex> lock{mtx_};
  return high_water_;
}

} // namespace kalshi
