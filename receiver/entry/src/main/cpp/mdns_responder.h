#pragma once

#include <arpa/inet.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mdns_protocol.h"

namespace hss::receiver {

enum class MdnsPublisherState {
  kStopped,
  kProbing,
  kPublished,
  kConflict,
  kError,
};

class MdnsResponder final {
 public:
  MdnsResponder() = default;
  ~MdnsResponder();
  MdnsResponder(const MdnsResponder&) = delete;
  MdnsResponder& operator=(const MdnsResponder&) = delete;

  bool Start(const std::string& address);
  void Stop();
  MdnsPublisherState state() const { return state_.load(); }

 private:
  int OpenSocket(in_addr address);
  void Run(in_addr address);
  bool SendPacket(const std::vector<std::byte>& packet);
  bool ReceivePacket(std::vector<std::byte>* packet, in_addr* source,
                     int timeoutMilliseconds);
  void CloseSocket();

  std::mutex lifecycle_mutex_;
  std::atomic<bool> desired_{false};
  std::atomic<int> socket_{-1};
  std::atomic<MdnsPublisherState> state_{MdnsPublisherState::kStopped};
  mdns::Ipv4Address published_address_{};
  std::thread worker_;
};

}  // namespace hss::receiver
