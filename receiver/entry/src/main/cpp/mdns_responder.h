#pragma once

#include "mdns_transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
  MdnsResponder();
  explicit MdnsResponder(std::unique_ptr<MdnsTransport> transport);
  ~MdnsResponder();
  MdnsResponder(const MdnsResponder&) = delete;
  MdnsResponder& operator=(const MdnsResponder&) = delete;

  bool Start(const std::string& address, std::uint32_t interfaceIndex);
  void Stop();
  MdnsPublisherState state() const { return state_.load(); }

 private:
  void Run();
  bool ObserveFor(int milliseconds);
  bool AcceptDatagramMetadata(const MdnsDatagram& datagram) const;
  bool AcceptMessageHopLimit(const MdnsDatagram& datagram,
                             mdns::MessageKind kind) const;
  bool HandleProbingDatagram(const MdnsDatagram& datagram);
  bool HandlePublishedDatagram(const MdnsDatagram& datagram);
  bool SendProbeLocked();
  bool SendAnnouncementLocked();
  void SendGoodbyeLocked();
  void FailLocked();
  void ConflictLocked(bool wasPublished);
  bool AllowQueryResponse(std::uint64_t nowMilliseconds);

  std::unique_ptr<MdnsTransport> transport_;
  std::mutex lifecycle_mutex_;
  std::atomic<bool> desired_{false};
  std::atomic<MdnsPublisherState> state_{MdnsPublisherState::kStopped};
  MdnsInterface selected_interface_{};
  bool goodbye_sent_ = false;
  bool response_window_initialized_ = false;
  std::uint64_t response_window_started_ms_ = 0;
  std::size_t responses_in_window_ = 0;
  std::thread worker_;
};

}  // namespace hss::receiver
