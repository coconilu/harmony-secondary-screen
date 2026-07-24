#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace hss::host {

class DataPlaneGate final {
 public:
  std::uint64_t Open() {
    allowed_ = false;
    std::scoped_lock sendLock(send_mutex_);
    const auto token = epoch_.fetch_add(1) + 1;
    allowed_ = true;
    return token;
  }
  void Revoke() {
    allowed_ = false;
    // Drain a packet already inside RunIfAllowed before completing revoke.
    std::scoped_lock sendLock(send_mutex_);
    epoch_.fetch_add(1);
  }
  std::optional<std::uint64_t> Capture() const {
    if (!allowed_.load()) return std::nullopt;
    const auto token = epoch_.load();
    return allowed_.load() && epoch_.load() == token ? std::optional(token) : std::nullopt;
  }
  bool CanSend(std::uint64_t token) const {
    return allowed_.load() && epoch_.load() == token;
  }
  template <typename Action>
  bool RunIfAllowed(std::uint64_t token, Action&& action) const {
    std::scoped_lock sendLock(send_mutex_);
    if (!allowed_.load() || epoch_.load() != token) return false;
    std::invoke(std::forward<Action>(action));
    return true;
  }

 private:
  mutable std::mutex send_mutex_;
  std::atomic<std::uint64_t> epoch_{0};
  std::atomic<bool> allowed_{false};
};

}  // namespace hss::host
