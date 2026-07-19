#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace hss::receiver {

class BoundedControlQueue final {
 public:
  explicit BoundedControlQueue(std::size_t capacity) : capacity_(capacity) {}

  void Push(std::string message) {
    std::scoped_lock lock(mutex_);
    if (capacity_ == 0) return;
    while (messages_.size() >= capacity_) messages_.pop_front();
    messages_.push_back(std::move(message));
  }

  bool TryPop(std::string* message) {
    if (message == nullptr) return false;
    std::scoped_lock lock(mutex_);
    if (messages_.empty()) return false;
    *message = std::move(messages_.front());
    messages_.pop_front();
    return true;
  }

  void Clear() {
    std::scoped_lock lock(mutex_);
    messages_.clear();
  }

  std::size_t size() const {
    std::scoped_lock lock(mutex_);
    return messages_.size();
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<std::string> messages_;
};

}  // namespace hss::receiver
