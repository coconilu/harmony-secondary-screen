#pragma once

#include "relay_protocol.h"

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace hwc::relay {

constexpr std::uint32_t kLanVideoMagic = 0x48535332U;  // HSS2
constexpr std::uint8_t kLanProtocolVersion = 2;
constexpr std::size_t kLanVideoHeaderSize = 32;
constexpr std::size_t kLanMaxUdpPayload = 1200;

struct LanSenderStats {
  std::uint64_t sent_frames = 0;
  std::uint64_t sent_bytes = 0;
  std::uint64_t sent_datagrams = 0;
  std::uint64_t send_errors = 0;
  std::uint64_t receiver_decoded = 0;
  std::uint64_t receiver_dropped = 0;
};

[[nodiscard]] std::vector<std::vector<std::uint8_t>> BuildLanVideoDatagrams(
    const LocalVideoMessage& video,
    std::uint32_t session_short);

class LanSender final {
 public:
  LanSender() = default;
  ~LanSender();
  LanSender(const LanSender&) = delete;
  LanSender& operator=(const LanSender&) = delete;

  [[nodiscard]] bool Configure(
      std::string receiver_address,
      std::string pairing_code,
      std::string* error_code);
  [[nodiscard]] bool SendAccessUnit(const LocalVideoMessage& video);
  [[nodiscard]] bool ConsumeKeyframeRequest();
  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] LanSenderStats stats() const noexcept;
  void Stop();

 private:
  [[nodiscard]] bool ConnectControl(
      const std::string& receiver_address,
      std::string* error_code);
  [[nodiscard]] bool PerformPairing(
      const std::string& pairing_code,
      std::string* error_code);
  [[nodiscard]] bool OpenVideoSocket(std::string* error_code);
  [[nodiscard]] bool SendControl(std::string_view json);
  [[nodiscard]] bool ReceiveControl(std::string* json, int timeout_ms);
  void ControlLoop();
  void HandleControl(std::string_view json);
  void CloseSockets();

  std::string receiver_address_;
  sockaddr_in receiver_video_address_{};
  std::uint16_t video_port_ = 0;
  std::uint32_t session_short_ = 0;
  std::atomic<SOCKET> control_socket_{INVALID_SOCKET};
  std::atomic<SOCKET> video_socket_{INVALID_SOCKET};
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> keyframe_requested_{false};
  std::thread control_worker_;
  std::mutex control_send_mutex_;
  std::mutex control_decode_mutex_;
  std::vector<std::uint8_t> control_buffer_;
  std::deque<std::string> control_frames_;

  std::atomic<std::uint64_t> sent_frames_{0};
  std::atomic<std::uint64_t> sent_bytes_{0};
  std::atomic<std::uint64_t> sent_datagrams_{0};
  std::atomic<std::uint64_t> send_errors_{0};
  std::atomic<std::uint64_t> receiver_decoded_{0};
  std::atomic<std::uint64_t> receiver_dropped_{0};
};

}  // namespace hwc::relay
