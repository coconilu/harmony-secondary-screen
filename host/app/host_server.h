#pragma once

#include "pointer_relay.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hss::host {

class HostServer final {
 public:
  HostServer();
  ~HostServer();
  HostServer(const HostServer&) = delete;
  HostServer& operator=(const HostServer&) = delete;

  bool Start(std::string* error);
  void Wait();
  void Stop();
  std::string pairing_code() const;

 private:
  void ControlLoop();
  void HandleClient(SOCKET client, sockaddr_in peer, std::string localAddress);
  void PipeLoop();
  void StatusLoop();
  bool SendControl(SOCKET client, std::string_view json);
  void SendVideoFrame(std::uint32_t frame, std::uint64_t timestampUs,
                      std::uint32_t flags, const std::vector<std::byte>& payload);
  void RotatePairingCode();
  void ClearSession();
  void MarkDisconnected();
  void ExpireResumeWindow();
  std::string SessionMessage() const;
  std::string StatusMessage() const;
  void RecordHostFrame(std::uint32_t frame, std::uint64_t captureUs, std::size_t bytes,
                       std::size_t fragments, bool keyframe);
  void RecordReceiverTelemetry(std::string_view json);

  HANDLE stop_event_ = nullptr;
  HANDLE keyframe_event_ = nullptr;
  WSADATA winsock_{};
  std::atomic<bool> started_{false};
  std::atomic<SOCKET> active_listener_{INVALID_SOCKET};
  std::atomic<SOCKET> active_client_{INVALID_SOCKET};
  std::thread control_thread_;
  std::thread pipe_thread_;
  std::thread status_thread_;

  mutable std::mutex state_mutex_;
  std::mutex metrics_mutex_;
  std::string pairing_code_;
  bool paired_ = false;
  bool resume_pending_ = false;
  std::chrono::steady_clock::time_point resume_deadline_{};
  std::array<std::byte, 16> session_id_{};
  std::uint32_t session_short_ = 0;
  sockaddr_in video_peer_{};
  SOCKET udp_socket_ = INVALID_SOCKET;
  PointerRelay pointer_relay_;
};

}  // namespace hss::host
