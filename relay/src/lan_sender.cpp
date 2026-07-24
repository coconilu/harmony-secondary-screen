#include "lan_sender.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <sstream>

namespace hwc::relay {
namespace {

constexpr std::uint16_t kControlPort = 44000;
constexpr std::size_t kMaxControlPayload = 64U * 1024U;

void WriteU16(std::uint8_t* target, const std::uint16_t value) {
  target[0] = static_cast<std::uint8_t>(value >> 8U);
  target[1] = static_cast<std::uint8_t>(value);
}

void WriteU32(std::uint8_t* target, const std::uint32_t value) {
  target[0] = static_cast<std::uint8_t>(value >> 24U);
  target[1] = static_cast<std::uint8_t>(value >> 16U);
  target[2] = static_cast<std::uint8_t>(value >> 8U);
  target[3] = static_cast<std::uint8_t>(value);
}

void WriteU64(std::uint8_t* target, const std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    target[7 - index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::uint32_t ReadU32(const std::uint8_t* source) {
  return (static_cast<std::uint32_t>(source[0]) << 24U) |
         (static_cast<std::uint32_t>(source[1]) << 16U) |
         (static_cast<std::uint32_t>(source[2]) << 8U) |
         static_cast<std::uint32_t>(source[3]);
}

void CloseSocket(std::atomic<SOCKET>* socket_value) {
  const SOCKET socket = socket_value->exchange(INVALID_SOCKET);
  if (socket != INVALID_SOCKET) {
    (void)shutdown(socket, SD_BOTH);
    (void)closesocket(socket);
  }
}

bool IsTrustedLanIpv4(
    const std::string& value,
    sockaddr_in* address) {
  if (address == nullptr) {
    return false;
  }
  *address = {};
  address->sin_family = AF_INET;
  if (InetPtonA(AF_INET, value.c_str(), &address->sin_addr) != 1) {
    return false;
  }
  const std::uint32_t host = ntohl(address->sin_addr.s_addr);
  const bool private_10 = (host & 0xFF000000U) == 0x0A000000U;
  const bool private_172 = (host & 0xFFF00000U) == 0xAC100000U;
  const bool private_192 = (host & 0xFFFF0000U) == 0xC0A80000U;
  const bool link_local = (host & 0xFFFF0000U) == 0xA9FE0000U;
  return private_10 || private_172 || private_192 || link_local;
}

std::vector<std::uint8_t> EncodeControl(
    const std::string_view json) {
  if (json.empty() || json.size() > kMaxControlPayload) {
    return {};
  }
  std::vector<std::uint8_t> frame(4 + json.size());
  WriteU32(frame.data(), static_cast<std::uint32_t>(json.size()));
  std::copy(json.begin(), json.end(), frame.begin() + 4);
  return frame;
}

bool SendAll(
    const SOCKET socket,
    const std::span<const std::uint8_t> bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const int count = send(
        socket,
        reinterpret_cast<const char*>(bytes.data() + sent),
        static_cast<int>(std::min<std::size_t>(
            bytes.size() - sent,
            static_cast<std::size_t>(std::numeric_limits<int>::max()))),
        0);
    if (count <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

}  // namespace

std::vector<std::vector<std::uint8_t>> BuildLanVideoDatagrams(
    const LocalVideoMessage& video,
    const std::uint32_t session_short) {
  if (session_short == 0 || video.payload.empty() ||
      video.payload.size() > kMaxVideoPayloadBytes) {
    return {};
  }
  const std::size_t fragment_count =
      (video.payload.size() + kLanMaxUdpPayload - 1) /
      kLanMaxUdpPayload;
  if (fragment_count == 0 ||
      fragment_count > std::numeric_limits<std::uint16_t>::max()) {
    return {};
  }

  std::vector<std::vector<std::uint8_t>> datagrams;
  datagrams.reserve(fragment_count);
  for (std::size_t fragment = 0; fragment < fragment_count; ++fragment) {
    const std::size_t offset = fragment * kLanMaxUdpPayload;
    const std::size_t payload_size = std::min(
        kLanMaxUdpPayload,
        video.payload.size() - offset);
    std::vector<std::uint8_t> packet(kLanVideoHeaderSize + payload_size);
    std::uint16_t flags = video.key_frame ? 0x0003U : 0U;
    if (fragment + 1 == fragment_count) {
      flags |= 0x0004U;
    }
    WriteU32(packet.data(), kLanVideoMagic);
    packet[4] = kLanProtocolVersion;
    packet[5] = static_cast<std::uint8_t>(kLanVideoHeaderSize);
    WriteU16(packet.data() + 6, flags);
    WriteU32(packet.data() + 8, session_short);
    WriteU32(packet.data() + 12, video.sequence);
    WriteU16(packet.data() + 16, static_cast<std::uint16_t>(fragment));
    WriteU16(
        packet.data() + 18,
        static_cast<std::uint16_t>(fragment_count));
    WriteU16(
        packet.data() + 20,
        static_cast<std::uint16_t>(payload_size));
    WriteU16(packet.data() + 22, 0);
    WriteU64(packet.data() + 24, video.timestamp_us);
    std::copy(
        video.payload.begin() + static_cast<std::ptrdiff_t>(offset),
        video.payload.begin() +
            static_cast<std::ptrdiff_t>(offset + payload_size),
        packet.begin() + static_cast<std::ptrdiff_t>(kLanVideoHeaderSize));
    datagrams.push_back(std::move(packet));
  }
  return datagrams;
}

LanSender::~LanSender() {
  Stop();
}

bool LanSender::Configure(
    std::string receiver_address,
    std::string pairing_code,
    std::string* const error_code) {
  Stop();
  if (error_code != nullptr) {
    error_code->clear();
  }
  sockaddr_in address{};
  if (!IsTrustedLanIpv4(receiver_address, &address)) {
    if (error_code != nullptr) *error_code = "receiver_address_not_allowed";
    return false;
  }
  if (pairing_code.size() != 6 ||
      !std::all_of(
          pairing_code.begin(),
          pairing_code.end(),
          [](const char value) { return value >= '0' && value <= '9'; })) {
    if (error_code != nullptr) *error_code = "pairing_code_invalid";
    return false;
  }

  receiver_address_ = std::move(receiver_address);
  receiver_video_address_ = address;
  if (!ConnectControl(receiver_address_, error_code) ||
      !PerformPairing(pairing_code, error_code) ||
      !OpenVideoSocket(error_code)) {
    CloseSockets();
    session_short_ = 0;
    return false;
  }

  sent_frames_ = 0;
  sent_bytes_ = 0;
  sent_datagrams_ = 0;
  send_errors_ = 0;
  receiver_decoded_ = 0;
  receiver_dropped_ = 0;
  keyframe_requested_ = true;
  connected_ = true;
  running_ = true;
  control_worker_ = std::thread(&LanSender::ControlLoop, this);
  return true;
}

bool LanSender::ConnectControl(
    const std::string& receiver_address,
    std::string* const error_code) {
  sockaddr_in address{};
  if (!IsTrustedLanIpv4(receiver_address, &address)) {
    if (error_code != nullptr) *error_code = "receiver_address_not_allowed";
    return false;
  }
  address.sin_port = htons(kControlPort);

  const SOCKET socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == INVALID_SOCKET) {
    if (error_code != nullptr) *error_code = "control_socket_failed";
    return false;
  }
  control_socket_ = socket_value;
  u_long nonblocking = 1;
  if (ioctlsocket(socket_value, FIONBIO, &nonblocking) == SOCKET_ERROR) {
    if (error_code != nullptr) *error_code = "control_socket_failed";
    return false;
  }
  const int connect_result = connect(
      socket_value,
      reinterpret_cast<const sockaddr*>(&address),
      sizeof(address));
  if (connect_result == SOCKET_ERROR &&
      WSAGetLastError() != WSAEWOULDBLOCK) {
    if (error_code != nullptr) *error_code = "receiver_unreachable";
    return false;
  }
  if (connect_result == SOCKET_ERROR) {
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(socket_value, &write_set);
    timeval timeout{3, 0};
    if (select(0, nullptr, &write_set, nullptr, &timeout) <= 0) {
      if (error_code != nullptr) *error_code = "receiver_connect_timeout";
      return false;
    }
    int socket_error = 0;
    int error_size = sizeof(socket_error);
    if (getsockopt(
            socket_value,
            SOL_SOCKET,
            SO_ERROR,
            reinterpret_cast<char*>(&socket_error),
            &error_size) == SOCKET_ERROR ||
        socket_error != 0) {
      if (error_code != nullptr) *error_code = "receiver_unreachable";
      return false;
    }
  }
  nonblocking = 0;
  if (ioctlsocket(socket_value, FIONBIO, &nonblocking) == SOCKET_ERROR) {
    if (error_code != nullptr) *error_code = "control_socket_failed";
    return false;
  }
  const int timeout_ms = 3000;
  (void)setsockopt(
      socket_value,
      SOL_SOCKET,
      SO_RCVTIMEO,
      reinterpret_cast<const char*>(&timeout_ms),
      sizeof(timeout_ms));
  (void)setsockopt(
      socket_value,
      SOL_SOCKET,
      SO_SNDTIMEO,
      reinterpret_cast<const char*>(&timeout_ms),
      sizeof(timeout_ms));
  return true;
}

bool LanSender::PerformPairing(
    const std::string& pairing_code,
    std::string* const error_code) {
  if (!SendControl(R"({"type":"hello","protocol":2})")) {
    if (error_code != nullptr) *error_code = "hello_send_failed";
    return false;
  }
  std::string hello;
  if (!ReceiveControl(&hello, 3000) ||
      JsonString(hello, "type") != "hello" ||
      JsonUnsigned(hello, "protocol") != kLanProtocolVersion) {
    if (error_code != nullptr) *error_code = "hello_rejected";
    return false;
  }
  const auto receiver_nonce = JsonString(hello, "receiverNonce");
  if (!receiver_nonce || receiver_nonce->size() < 16) {
    if (error_code != nullptr) *error_code = "hello_rejected";
    return false;
  }

  std::ostringstream pair;
  pair << "{\"type\":\"pair\",\"protocol\":2,\"pairingCode\":\""
       << EscapeJson(pairing_code)
       << "\",\"senderNonce\":\"" << GenerateTokenHex(16)
       << "\",\"receiverNonce\":\"" << EscapeJson(*receiver_nonce)
       << "\",\"codec\":\"video/avc\",\"avcFormat\":\"annexb\","
          "\"width\":1280,\"height\":720,\"fps\":30}";
  if (!SendControl(pair.str())) {
    if (error_code != nullptr) *error_code = "pair_send_failed";
    return false;
  }
  std::string session;
  if (!ReceiveControl(&session, 3000)) {
    if (error_code != nullptr) *error_code = "pair_timeout";
    return false;
  }
  if (JsonString(session, "type") == "error") {
    if (error_code != nullptr) {
      *error_code = JsonString(session, "code").value_or("pairing_failed");
    }
    return false;
  }
  const auto session_short = JsonUnsigned(session, "sessionShort");
  const auto video_port = JsonUnsigned(session, "videoPort");
  if (JsonString(session, "type") != "session" ||
      JsonUnsigned(session, "protocol") != kLanProtocolVersion ||
      !session_short || *session_short == 0 ||
      *session_short > std::numeric_limits<std::uint32_t>::max() ||
      !video_port || *video_port < 1024 ||
      *video_port > std::numeric_limits<std::uint16_t>::max() ||
      JsonUnsigned(session, "width") != 1280 ||
      JsonUnsigned(session, "height") != 720 ||
      JsonUnsigned(session, "fps") != 30) {
    if (error_code != nullptr) *error_code = "invalid_session";
    return false;
  }
  session_short_ = static_cast<std::uint32_t>(*session_short);
  video_port_ = static_cast<std::uint16_t>(*video_port);
  receiver_video_address_.sin_port = htons(video_port_);
  return true;
}

bool LanSender::OpenVideoSocket(std::string* const error_code) {
  const SOCKET socket_value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_value == INVALID_SOCKET) {
    if (error_code != nullptr) *error_code = "video_socket_failed";
    return false;
  }
  video_socket_ = socket_value;
  const int send_buffer = 1024 * 1024;
  (void)setsockopt(
      socket_value,
      SOL_SOCKET,
      SO_SNDBUF,
      reinterpret_cast<const char*>(&send_buffer),
      sizeof(send_buffer));
  if (connect(
          socket_value,
          reinterpret_cast<const sockaddr*>(&receiver_video_address_),
          sizeof(receiver_video_address_)) == SOCKET_ERROR) {
    if (error_code != nullptr) *error_code = "video_connect_failed";
    return false;
  }
  return true;
}

bool LanSender::SendAccessUnit(const LocalVideoMessage& video) {
  if (!connected_.load()) {
    return false;
  }
  const auto packets = BuildLanVideoDatagrams(video, session_short_);
  const SOCKET socket_value = video_socket_.load();
  if (packets.empty() || socket_value == INVALID_SOCKET) {
    ++send_errors_;
    return false;
  }
  for (const auto& packet : packets) {
    const int sent = send(
        socket_value,
        reinterpret_cast<const char*>(packet.data()),
        static_cast<int>(packet.size()),
        0);
    if (sent != static_cast<int>(packet.size())) {
      ++send_errors_;
      connected_ = false;
      return false;
    }
    ++sent_datagrams_;
  }
  ++sent_frames_;
  sent_bytes_ += video.payload.size();
  return true;
}

bool LanSender::ConsumeKeyframeRequest() {
  return keyframe_requested_.exchange(false);
}

bool LanSender::connected() const noexcept {
  return connected_.load();
}

LanSenderStats LanSender::stats() const noexcept {
  return {
      .sent_frames = sent_frames_.load(),
      .sent_bytes = sent_bytes_.load(),
      .sent_datagrams = sent_datagrams_.load(),
      .send_errors = send_errors_.load(),
      .receiver_decoded = receiver_decoded_.load(),
      .receiver_dropped = receiver_dropped_.load()};
}

bool LanSender::SendControl(const std::string_view json) {
  const std::vector<std::uint8_t> frame = EncodeControl(json);
  const SOCKET socket_value = control_socket_.load();
  if (frame.empty() || socket_value == INVALID_SOCKET) {
    return false;
  }
  std::scoped_lock lock(control_send_mutex_);
  return SendAll(socket_value, frame);
}

bool LanSender::ReceiveControl(
    std::string* const json,
    const int timeout_ms) {
  if (json == nullptr) {
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::scoped_lock lock(control_decode_mutex_);
      if (!control_frames_.empty()) {
        *json = std::move(control_frames_.front());
        control_frames_.pop_front();
        return true;
      }
    }
    const SOCKET socket_value = control_socket_.load();
    if (socket_value == INVALID_SOCKET) {
      return false;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_value, &read_set);
    timeval wait{0, 100'000};
    const int ready = select(0, &read_set, nullptr, nullptr, &wait);
    if (ready == SOCKET_ERROR) {
      return false;
    }
    if (ready == 0) {
      continue;
    }
    std::array<std::uint8_t, 8192> buffer{};
    const int count = recv(
        socket_value,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0);
    if (count <= 0) {
      return false;
    }
    std::scoped_lock lock(control_decode_mutex_);
    if (control_buffer_.size() + static_cast<std::size_t>(count) >
        kMaxControlPayload + 4U) {
      return false;
    }
    control_buffer_.insert(
        control_buffer_.end(),
        buffer.begin(),
        buffer.begin() + count);
    while (control_buffer_.size() >= 4) {
      const std::uint32_t length = ReadU32(control_buffer_.data());
      if (length == 0 || length > kMaxControlPayload) {
        return false;
      }
      if (control_buffer_.size() < 4U + length) {
        break;
      }
      control_frames_.emplace_back(
          reinterpret_cast<const char*>(control_buffer_.data() + 4),
          length);
      control_buffer_.erase(
          control_buffer_.begin(),
          control_buffer_.begin() +
              static_cast<std::ptrdiff_t>(4U + length));
    }
  }
  return false;
}

void LanSender::ControlLoop() {
  auto next_ping = std::chrono::steady_clock::now();
  while (running_.load()) {
    std::string control;
    if (ReceiveControl(&control, 100)) {
      HandleControl(control);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_ping) {
      const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
          now.time_since_epoch()).count();
      if (!SendControl(
              "{\"type\":\"ping\",\"senderSendUs\":" +
              std::to_string(timestamp) + "}")) {
        connected_ = false;
        running_ = false;
        break;
      }
      next_ping = now + std::chrono::seconds(1);
    }
  }
}

void LanSender::HandleControl(const std::string_view json) {
  const auto type = JsonString(json, "type");
  if (type == "keyframe") {
    keyframe_requested_ = true;
    return;
  }
  if (type == "telemetry") {
    const auto decoded = JsonUnsigned(json, "framesDecoded");
    const auto dropped = JsonUnsigned(json, "framesDropped");
    if (decoded) receiver_decoded_ = *decoded;
    if (dropped) receiver_dropped_ = *dropped;
    return;
  }
  if (type == "error") {
    connected_ = false;
    running_ = false;
  }
}

void LanSender::Stop() {
  const bool was_connected = connected_.exchange(false);
  running_ = false;
  if (was_connected && control_socket_.load() != INVALID_SOCKET) {
    (void)SendControl(
        R"({"type":"stop","reason":"user_stopped_capture"})");
  }
  CloseSockets();
  if (control_worker_.joinable() &&
      control_worker_.get_id() != std::this_thread::get_id()) {
    control_worker_.join();
  }
  session_short_ = 0;
  video_port_ = 0;
  receiver_address_.clear();
  keyframe_requested_ = false;
  {
    std::scoped_lock lock(control_decode_mutex_);
    control_buffer_.clear();
    control_frames_.clear();
  }
}

void LanSender::CloseSockets() {
  CloseSocket(&control_socket_);
  CloseSocket(&video_socket_);
}

}  // namespace hwc::relay
