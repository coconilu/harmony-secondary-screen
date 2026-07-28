#include "receiver_session.h"

#include "avc_decoder_input.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace hss::receiver {
namespace {

constexpr std::uint16_t kControlPort = 44000;
constexpr unsigned int kLogDomain = 0x0000;
constexpr const char* kLogTag = "HSSReceiver";

std::uint64_t ClockMicroseconds() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void CloseSocket(std::atomic<int>* socketValue) {
  const int descriptor = socketValue->exchange(-1);
  if (descriptor >= 0) {
    shutdown(descriptor, SHUT_RDWR);
    close(descriptor);
  }
}

bool ValidIpv4(const std::string& value, in_addr* address) {
  return inet_pton(AF_INET, value.c_str(), address) == 1;
}

bool IsCurrentWifiIpv4(const std::string& value, in_addr* address,
                       std::uint32_t* interfaceIndex = nullptr) {
  if (!ValidIpv4(value, address)) return false;
  const std::uint32_t hostOrder = ntohl(address->s_addr);
  const std::uint8_t first = static_cast<std::uint8_t>(hostOrder >> 24U);
  const std::uint8_t second = static_cast<std::uint8_t>((hostOrder >> 16U) & 0xffU);
  const bool trustedLan =
      first == 10U || (first == 172U && second >= 16U && second <= 31U) ||
      (first == 192U && second == 168U) || (first == 169U && second == 254U);
  if (!trustedLan) {
    return false;
  }
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return false;
  bool matched = false;
  for (const ifaddrs* item = interfaces; item != nullptr; item = item->ifa_next) {
    if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET ||
        item->ifa_name == nullptr || (item->ifa_flags & IFF_UP) == 0 ||
        (item->ifa_flags & IFF_LOOPBACK) != 0 ||
        std::strncmp(item->ifa_name, "wlan", 4) != 0) {
      continue;
    }
    const auto* candidate = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
    if (candidate->sin_addr.s_addr == address->s_addr) {
      const unsigned int index = if_nametoindex(item->ifa_name);
      matched = index != 0U;
      if (matched && interfaceIndex != nullptr) {
        *interfaceIndex = static_cast<std::uint32_t>(index);
      }
      break;
    }
  }
  freeifaddrs(interfaces);
  return matched;
}

bool RandomBytes(void* output, std::size_t size) {
  const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) return false;
  auto* target = static_cast<std::byte*>(output);
  std::size_t readCount = 0;
  while (readCount < size) {
    const ssize_t count = read(descriptor, target + readCount, size - readCount);
    if (count > 0) {
      readCount += static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      close(descriptor);
      return false;
    }
  }
  close(descriptor);
  return true;
}

std::string RandomHex(std::size_t byteCount) {
  std::vector<std::byte> bytes(byteCount);
  if (!RandomBytes(bytes.data(), bytes.size())) return {};
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto value : bytes) {
    output << std::setw(2) << static_cast<unsigned int>(std::to_integer<std::uint8_t>(value));
  }
  return output.str();
}

std::string Ipv4Text(const in_addr& address) {
  std::array<char, INET_ADDRSTRLEN> text{};
  return inet_ntop(AF_INET, &address, text.data(), text.size()) == nullptr ? "" : text.data();
}

bool Hex(const std::string& value, std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

bool SenderIdValid(const std::string& value) {
  return value.size() >= 16U && value.size() <= 64U &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f') || character == '-';
         });
}

}  // namespace

ReceiverSession& ReceiverSession::Instance() {
  static ReceiverSession session;
  return session;
}

ReceiverSession::~ReceiverSession() {
  Stop();
}

bool ReceiverSession::Start(std::string listenAddress) {
  in_addr address{};
  std::uint32_t interfaceIndex = 0;
  if (!IsCurrentWifiIpv4(listenAddress, &address, &interfaceIndex)) {
    SetState("error", "这个地址不是平板当前使用的 Wi-Fi 地址", false, false);
    return false;
  }
  Stop();
  std::string generatedDeviceId;
  {
    std::scoped_lock lock(state_mutex_);
    if (device_id_.empty()) generatedDeviceId = RandomHex(16);
  }
  if (!generatedDeviceId.empty()) {
    std::scoped_lock lock(state_mutex_);
    if (device_id_.empty()) device_id_ = std::move(generatedDeviceId);
  }
  {
    std::scoped_lock lock(state_mutex_);
    if (device_id_.empty()) {
      state_ = "error";
      detail_ = "无法生成设备身份";
      listening_ = false;
      connected_ = false;
      return false;
    }
    listen_address_ = std::move(listenAddress);
    listen_interface_index_ = interfaceIndex;
    paired_address_.clear();
  }
  frames_decoded_ = 0;
  frames_dropped_ = 0;
  received_frames_ = 0;
  received_bytes_ = 0;
  decoder_resync_events_ = 0;
  keyframe_requests_sent_ = 0;
  decoder_recovery_.Reset();
  desired_ = true;
  SetState("starting", "正在准备接收电脑画面", false, false);
  worker_ = std::thread(&ReceiverSession::NetworkLoop, this);
  return true;
}

void ReceiverSession::Stop() {
  desired_ = false;
  address_responder_.Stop();
  CloseSockets();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
    worker_.join();
  }
  websocket_decoder_.Reset();
  telemetry_queue_.Clear();
  decoder_recovery_.Reset();
  SetState("idle", "尚未开始接收画面", false, false);
}

bool ReceiverSession::ConfigureTrust(std::string deviceId, std::string senderId,
                                     std::string credential, std::uint64_t version) {
  if (!Hex(deviceId, 32) ||
      ((!senderId.empty() || !credential.empty()) &&
       (!SenderIdValid(senderId) || !Hex(credential, 64)))) {
    return false;
  }
  std::scoped_lock lock(state_mutex_);
  device_id_ = std::move(deviceId);
  trusted_sender_id_ = std::move(senderId);
  trusted_credential_ = std::move(credential);
  pairing_record_version_ = version;
  return true;
}

bool ReceiverSession::AuthorizeQr(std::string sessionId, std::string token,
                                  std::int64_t expiresAtMs) {
  const auto now = std::chrono::system_clock::now();
  const auto expiry = std::chrono::system_clock::time_point(
      std::chrono::milliseconds(expiresAtMs));
  if (!Hex(sessionId, 32) || !Hex(token, 64) || expiry <= now ||
      expiry > now + std::chrono::seconds(65)) {
    return false;
  }
  std::scoped_lock lock(state_mutex_);
  pending_session_id_ = std::move(sessionId);
  pending_token_ = std::move(token);
  pending_short_code_.clear();
  pairing_expires_at_ = expiry;
  detail_ = "一次性扫码授权已就绪，等待 Edge 连接";
  return true;
}

bool ReceiverSession::AuthorizeShortCode(std::string shortCode) {
  if (shortCode.size() != 6U ||
      !std::all_of(shortCode.begin(), shortCode.end(),
                   [](unsigned char character) { return std::isdigit(character) != 0; })) {
    return false;
  }
  std::scoped_lock lock(state_mutex_);
  pending_session_id_.clear();
  pending_token_.clear();
  pending_short_code_ = std::move(shortCode);
  pairing_expires_at_ = std::chrono::system_clock::now() + std::chrono::seconds(60);
  detail_ = "一次性短码授权已就绪，等待 Edge 连接";
  return true;
}

void ReceiverSession::ForgetDevice() {
  {
    std::scoped_lock lock(state_mutex_);
    trusted_sender_id_.clear();
    trusted_credential_.clear();
    pending_session_id_.clear();
    pending_token_.clear();
    pending_short_code_.clear();
    ++pairing_record_version_;
    detail_ = "已忘记电脑，需要重新扫码配对";
  }
  CloseControlSocket();
}

StatusSnapshot ReceiverSession::Status() const {
  std::scoped_lock lock(state_mutex_);
  const auto publisherState = address_responder_.state();
  return {state_, detail_, listen_address_, paired_address_, device_id_, listening_,
          connected_, !trusted_sender_id_.empty(),
          publisherState == MdnsPublisherState::kPublished,
          publisherState == MdnsPublisherState::kConflict,
          publisherState == MdnsPublisherState::kError, frames_decoded_.load(),
          frames_dropped_.load(), received_frames_.load()};
}

PairingRecord ReceiverSession::Pairing() const {
  std::scoped_lock lock(state_mutex_);
  return {device_id_, trusted_sender_id_, trusted_credential_, pairing_record_version_};
}

std::vector<std::string> ReceiverSession::WifiAddresses() const {
  std::vector<std::string> addresses;
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return addresses;
  for (const ifaddrs* item = interfaces; item != nullptr; item = item->ifa_next) {
    if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET ||
        item->ifa_name == nullptr || (item->ifa_flags & IFF_UP) == 0 ||
        (item->ifa_flags & IFF_LOOPBACK) != 0 ||
        std::strncmp(item->ifa_name, "wlan", 4) != 0) {
      continue;
    }
    const auto* candidate = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
    const std::string text = Ipv4Text(candidate->sin_addr);
    in_addr verified{};
    if (IsCurrentWifiIpv4(text, &verified)) addresses.push_back(text);
  }
  freeifaddrs(interfaces);
  return addresses;
}

void ReceiverSession::SetState(std::string state, std::string detail, bool listening,
                               bool connected) {
  std::scoped_lock lock(state_mutex_);
  state_ = std::move(state);
  detail_ = std::move(detail);
  listening_ = listening;
  connected_ = connected;
}

void ReceiverSession::NetworkLoop() {
  if (!OpenListener()) {
    if (desired_) SetState("error", "无法开始接收，请检查 Wi-Fi 后重试", false, false);
    CloseSockets();
    return;
  }
  std::string addressText;
  std::uint32_t interfaceIndex = 0;
  {
    std::scoped_lock lock(state_mutex_);
    addressText = listen_address_;
    interfaceIndex = listen_interface_index_;
  }
  address_responder_.Start(addressText, interfaceIndex);
  SetState("listening", "等待电脑发送画面；首次使用请先连接电脑", true, false);
  while (desired_) {
    if (!ContinueWithCurrentAddress()) break;
    AcceptWebSocket();
    CloseControlSocket();
    websocket_decoder_.Reset();
    if (desired_) {
      SetState("listening", "等待电脑发送画面；首次使用请先连接电脑", true, false);
    }
  }
  address_responder_.Stop();
  CloseSockets();
  telemetry_queue_.Clear();
  decoder_recovery_.Reset();
}

bool ReceiverSession::OpenListener() {
  std::string addressText;
  {
    std::scoped_lock lock(state_mutex_);
    addressText = listen_address_;
  }
  in_addr address{};
  if (!IsCurrentWifiIpv4(addressText, &address)) return false;
  int reuse = 1;

  const int tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (tcp < 0) return false;
  listener_socket_ = tcp;
  setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in tcpAddress{};
  tcpAddress.sin_family = AF_INET;
  tcpAddress.sin_addr = address;
  tcpAddress.sin_port = htons(kControlPort);
  if (bind(tcp, reinterpret_cast<sockaddr*>(&tcpAddress), sizeof(tcpAddress)) != 0 ||
      listen(tcp, 1) != 0) {
    return false;
  }

  return true;
}

bool ReceiverSession::AcceptWebSocket() {
  const int listener = listener_socket_.load();
  if (listener < 0) return false;
  fd_set set;
  FD_ZERO(&set);
  FD_SET(listener, &set);
  timeval timeout{0, 100'000};
  const int ready = select(listener + 1, &set, nullptr, nullptr, &timeout);
  if (ready <= 0) return false;

  sockaddr_in peer{};
  socklen_t peerSize = sizeof(peer);
  const int client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerSize);
  if (client < 0) return false;
  const std::uint32_t peerHost = ntohl(peer.sin_addr.s_addr);
  const std::uint8_t first = static_cast<std::uint8_t>(peerHost >> 24U);
  const std::uint8_t second = static_cast<std::uint8_t>((peerHost >> 16U) & 0xffU);
  const bool privatePeer =
      first == 10U || (first == 172U && second >= 16U && second <= 31U) ||
      (first == 192U && second == 168U) || (first == 169U && second == 254U);
  if (!privatePeer) {
    close(client);
    return false;
  }
  control_socket_ = client;
  {
    std::scoped_lock lock(state_mutex_);
    paired_address_ = Ipv4Text(peer.sin_addr);
  }
  if (!ReadUpgrade(client) || !AuthenticateConnection()) return false;
  return RunConnectedSession();
}

bool ReceiverSession::ReadUpgrade(int client) {
  std::string request;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  if (!websocket::ReadUpgradeRequest(client, deadline, &request)) return false;
  std::string response;
  if (!websocket::BuildUpgradeResponse(request, &response)) return false;
  return send(client, response.data(), response.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(response.size());
}

bool ReceiverSession::AuthenticateConnection() {
  const int socket = control_socket_.load();
  if (socket < 0) return false;
  std::array<std::byte, 8192> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (desired_ && std::chrono::steady_clock::now() < deadline) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout{0, 100'000};
    const int ready = select(socket + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready < 0 && errno != EINTR) return false;
    if (ready <= 0) continue;
    const ssize_t count = recv(socket, buffer.data(), buffer.size(), 0);
    if (count <= 0) return false;
    std::vector<websocket::Message> messages;
    if (!websocket_decoder_.Push(buffer.data(), static_cast<std::size_t>(count), &messages)) {
      return false;
    }
    for (const auto& message : messages) {
      if (message.opcode != websocket::Opcode::kText) return false;
      const std::string json(reinterpret_cast<const char*>(message.payload.data()),
                             message.payload.size());
      if (protocol::JsonInteger(json, "protocol") != protocol::kVersion) {
        SendControl(R"({"type":"error","protocol":4,"code":"protocol_mismatch"})");
        return false;
      }
      const auto type = protocol::JsonString(json, "type");
      if (type == "pair") {
        const std::string sessionId = protocol::JsonString(json, "sessionId").value_or("");
        const std::string token = protocol::JsonString(json, "token").value_or("");
        const std::string senderId = protocol::JsonString(json, "senderId").value_or("");
        std::string error;
        std::string deviceId;
        std::string credential;
        {
          std::scoped_lock lock(state_mutex_);
          const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
          const auto expiresAt = std::chrono::duration_cast<std::chrono::milliseconds>(
              pairing_expires_at_.time_since_epoch()).count();
          switch (protocol::EvaluatePairingAuthorization(
              sessionId, token, pending_session_id_, pending_token_,
              pending_short_code_, consumed_session_id_, expiresAt, now)) {
            case protocol::PairingAuthorizationResult::kReplayed:
              error = "authorization_replayed";
              break;
            case protocol::PairingAuthorizationResult::kExpired:
              error = "authorization_expired";
              break;
            case protocol::PairingAuthorizationResult::kMismatch:
              error = "pairing_failed";
              break;
            case protocol::PairingAuthorizationResult::kAccepted:
              break;
          }
          if (error.empty() && !SenderIdValid(senderId)) {
            error = "pairing_failed";
          }
          if (error.empty()) {
            credential = RandomHex(32);
            if (credential.empty()) {
              error = "pairing_failed";
            } else {
              if (device_id_.empty()) device_id_ = RandomHex(16);
              deviceId = device_id_;
              trusted_sender_id_ = senderId;
              trusted_credential_ = credential;
              consumed_session_id_ = sessionId;
              pending_session_id_.clear();
              pending_token_.clear();
              pending_short_code_.clear();
              ++pairing_record_version_;
            }
          }
        }
        if (!error.empty()) {
          return SendControl("{\"type\":\"error\",\"protocol\":4,\"code\":\"" + error + "\"}") &&
                 false;
        }
        std::ostringstream reply;
        reply << "{\"type\":\"paired\",\"protocol\":4,\"deviceId\":\""
              << protocol::EscapeJson(deviceId) << "\",\"credential\":\""
              << protocol::EscapeJson(credential) << "\"}";
        SendControl(reply.str());
        SetState("paired", "电脑连接成功，下次使用无需再次扫码", true, false);
        return false;
      }
      if (type != "auth") {
        SendControl(R"({"type":"error","protocol":4,"code":"not_paired"})");
        return false;
      }
      const std::string senderId = protocol::JsonString(json, "senderId").value_or("");
      const std::string deviceId = protocol::JsonString(json, "deviceId").value_or("");
      const std::string credential = protocol::JsonString(json, "credential").value_or("");
      const auto epoch = protocol::JsonInteger(json, "sourceEpoch");
      bool trusted = false;
      bool stale = false;
      {
        std::scoped_lock lock(state_mutex_);
        trusted = !trusted_sender_id_.empty() && senderId == trusted_sender_id_ &&
                  deviceId == device_id_ && credential == trusted_credential_;
        stale = !epoch || *epoch <= 0 || *epoch > UINT32_MAX ||
                !protocol::IsAcceptedSourceEpoch(static_cast<std::uint32_t>(*epoch),
                                                 latest_source_epoch_);
        if (trusted && !stale) {
          const auto accepted = static_cast<std::uint32_t>(*epoch);
          latest_source_epoch_ = accepted;
          active_source_epoch_ = accepted;
        }
      }
      if (!trusted) {
        SendControl(R"({"type":"error","protocol":4,"code":"identity_mismatch"})");
        return false;
      }
      if (stale) {
        SendControl(R"({"type":"error","protocol":4,"code":"epoch_stale"})");
        return false;
      }
      const bool codecValid =
          protocol::JsonString(json, "codec") == "video/avc" &&
          protocol::JsonString(json, "avcFormat") == "annexb" &&
          protocol::JsonInteger(json, "width") == 1280 &&
          protocol::JsonInteger(json, "height") == 720 &&
          protocol::JsonInteger(json, "fps") == 60;
      if (!codecValid) {
        SendControl(R"({"type":"error","protocol":4,"code":"codec_unsupported"})");
        return false;
      }
      FlushDecoder();
      std::ostringstream reply;
      reply << "{\"type\":\"ready\",\"protocol\":4,\"sourceEpoch\":" << *epoch << "}";
      if (!SendControl(reply.str())) return false;
      SetState("connected", "电脑已连接，正在准备播放画面", true, true);
      return true;
    }
  }
  return false;
}

bool ReceiverSession::RunConnectedSession() {
  const int socket = control_socket_.load();
  if (socket < 0) return false;
  std::array<std::byte, 64U * 1024U> buffer{};
  while (desired_) {
    if (!ContinueWithCurrentAddress()) return false;
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout{0, 100'000};
    const int ready = select(socket + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready < 0 && errno != EINTR) {
      OH_LOG_Print(LOG_APP, LOG_WARN, kLogDomain, kLogTag,
                   "Receiver WebSocket select failed, errno=%{public}d", errno);
      return false;
    }
    if (ready > 0) {
      const ssize_t count = recv(socket, buffer.data(), buffer.size(), 0);
      if (count <= 0) {
        const int readError = count < 0 ? errno : 0;
        OH_LOG_Print(LOG_APP, LOG_WARN, kLogDomain, kLogTag,
                     "Receiver WebSocket read ended, result=%{public}ld errno=%{public}d",
                     static_cast<long>(count), readError);
        return false;
      }
      std::vector<websocket::Message> messages;
      if (!websocket_decoder_.Push(buffer.data(), static_cast<std::size_t>(count), &messages)) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kLogDomain, kLogTag,
                     "Receiver WebSocket frame decode failed");
        return false;
      }
      for (const auto& message : messages) {
        if (message.opcode == websocket::Opcode::kText) {
          const std::string json(reinterpret_cast<const char*>(message.payload.data()),
                                 message.payload.size());
          if (!HandleControl(json)) return false;
        } else if (message.opcode == websocket::Opcode::kBinary) {
          HandleVideo(message.payload.data(), message.payload.size());
        } else if (message.opcode == websocket::Opcode::kPing) {
          const std::string payload(reinterpret_cast<const char*>(message.payload.data()),
                                    message.payload.size());
          const auto pong = websocket::EncodeFrame(websocket::Opcode::kPong, payload);
          if (send(socket, pong.data(), pong.size(), MSG_NOSIGNAL) !=
              static_cast<ssize_t>(pong.size())) return false;
        } else if (message.opcode == websocket::Opcode::kClose) {
          OH_LOG_Print(LOG_APP, LOG_INFO, kLogDomain, kLogTag,
                       "Receiver WebSocket peer requested close");
          return false;
        }
      }
    }
    if (decoder_recovery_.ConsumeKeyFrameRequest()) {
      if (!SendControl(R"({"type":"keyframe","protocol":4,"reason":"loss_flush_or_session_start","requireCodecConfig":true})")) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kLogDomain, kLogTag,
                     "Receiver WebSocket keyframe request send failed");
        return false;
      }
      ++keyframe_requests_sent_;
    }
    std::string telemetry;
    if (telemetry_queue_.TryPop(&telemetry) && !SendControl(telemetry)) {
      OH_LOG_Print(LOG_APP, LOG_WARN, kLogDomain, kLogTag,
                   "Receiver WebSocket telemetry send failed");
      return false;
    }
  }
  return false;
}

bool ReceiverSession::ContinueWithCurrentAddress() {
  std::string addressText;
  std::uint32_t selectedInterfaceIndex = 0;
  {
    std::scoped_lock lock(state_mutex_);
    addressText = listen_address_;
    selectedInterfaceIndex = listen_interface_index_;
  }
  in_addr address{};
  std::uint32_t currentInterfaceIndex = 0;
  if (IsCurrentWifiIpv4(addressText, &address, &currentInterfaceIndex) &&
      currentInterfaceIndex == selectedInterfaceIndex) {
    return true;
  }
  address_responder_.Stop();
  desired_ = false;
  SetState("error", "Wi-Fi 地址已变化，请重新确认后开始接收", false, false);
  return false;
}

bool ReceiverSession::SendControl(std::string_view json) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  std::unique_lock<std::timed_mutex> lock(send_mutex_, std::defer_lock);
  if (!lock.try_lock_until(deadline)) return false;
  const int descriptor = control_socket_.load();
  if (descriptor < 0) return false;
  const auto frame = websocket::EncodeFrame(websocket::Opcode::kText, json);
  if (frame.empty()) return false;
  std::size_t sent = 0;
  while (sent < frame.size()) {
    const ssize_t count = send(descriptor, frame.data() + sent, frame.size() - sent,
                               MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(descriptor, &writeSet);
    timeval timeout{static_cast<time_t>(remaining.count() / 1'000'000),
                    static_cast<suseconds_t>(remaining.count() % 1'000'000)};
    const int ready = select(descriptor + 1, nullptr, &writeSet, nullptr, &timeout);
    if (ready <= 0) return false;
  }
  return true;
}

bool ReceiverSession::HandleControl(std::string_view json) {
  if (protocol::JsonInteger(json, "protocol") != protocol::kVersion) return false;
  const auto type = protocol::JsonString(json, "type");
  if (type == "ping") {
    const auto at = protocol::JsonInteger(json, "at");
    if (!at) return false;
    std::ostringstream pong;
    pong << "{\"type\":\"pong\",\"protocol\":4,\"at\":" << *at << "}";
    return SendControl(pong.str());
  }
  return type != "close";
}

void ReceiverSession::HandleVideo(const std::byte* data, std::size_t size) {
  const auto header = protocol::DecodeVideoHeader(data, size);
  std::uint32_t activeEpoch = 0;
  {
    std::scoped_lock lock(state_mutex_);
    activeEpoch = active_source_epoch_;
  }
  if (!header || header->sourceEpoch != activeEpoch) {
    ++frames_dropped_;
    return;
  }
  ++received_frames_;
  received_bytes_ += header->payloadLength;
  if (!decoder_callback_gate_.CallbacksAllowed() || decoder_.load() == nullptr) {
    ++frames_dropped_;
    return;
  }
  const auto recoveryState = decoder_recovery_.state();
  const bool keyframe = (header->flags & protocol::kKeyframe) != 0;
  std::vector<std::byte> bytes(data + protocol::kHeaderSize,
                               data + protocol::kHeaderSize + header->payloadLength);
  if (recoveryState == DecoderRecoveryState::kNeedsCodecData) {
    auto recovery = SplitAvcRecoveryInput(bytes);
    if (!keyframe || !recovery.complete()) {
      ++frames_dropped_;
      RequestKeyframe();
    } else {
      DecodedInput configInput{std::move(recovery.codecData), header->timestampUs,
                               DecoderInputKind::kCodecData};
      DecodedInput syncInput{std::move(recovery.syncFrame), header->timestampUs,
                             DecoderInputKind::kSyncFrame};
      if (!SubmitRecovery(std::move(configInput), std::move(syncInput))) {
        RequestKeyframe();
      }
    }
  } else if (recoveryState == DecoderRecoveryState::kNeedsSyncFrame && !keyframe) {
    ++frames_dropped_;
    RequestKeyframe();
  } else {
    DecodedInput frame{std::move(bytes), header->timestampUs,
                       keyframe ? DecoderInputKind::kSyncFrame
                                : DecoderInputKind::kFrame};
    SubmitFrame(std::move(frame));
  }
}

void ReceiverSession::RequestKeyframe() {
  decoder_recovery_.RequestKeyFrame();
}

void ReceiverSession::CloseControlSocket() {
  CloseSocket(&control_socket_);
}

void ReceiverSession::CloseSockets() {
  CloseSocket(&control_socket_);
  CloseSocket(&listener_socket_);
}

DecoderRuntimeSnapshot ReceiverSession::DecoderRuntimeLocked() const {
  return {decoder_callback_gate_.state(), decoder_.load() != nullptr};
}

bool ReceiverSession::ApplyLifecycleDecisionLocked(
    ReceiverLifecycleDecision decision) {
  if (!decision.accepted) return false;
  if (decision.stopDecoder) DestroyDecoderLocked();
  if (!decision.startDecoder) return false;
  return CreateDecoderLocked();
}

bool ReceiverSession::CreateDecoderLocked() {
  const bool connected = Status().connected;
  const auto fail = [this, connected](const char* operation, int32_t errorCode) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, kLogTag,
                 "AVCodec initialization failed at %{public}s, error=%{public}d",
                 operation, errorCode);
    SetState("error",
             std::string("视频播放准备失败：") + operation + "（错误码 " +
                 std::to_string(errorCode) + "）",
             connected, connected);
  };

  if (!lifecycle_state_.DecoderShouldRun()) {
    fail("Surface", AV_ERR_INVALID_VAL);
    return false;
  }
  decoder_callback_gate_.SetState(DecoderLifecycleState::kStarting);
  OH_AVCodec* decoder = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
  if (decoder == nullptr) {
    fail("CreateByMime", AV_ERR_UNSUPPORT);
    decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
    return false;
  }
  OH_AVCodecCallback callbacks{OnCodecError, OnCodecStreamChanged,
                                OnCodecNeedInput, OnCodecOutput};
  const OH_AVErrCode registerCallback = OH_VideoDecoder_RegisterCallback(decoder, callbacks, this);
  if (registerCallback != AV_ERR_OK) {
    fail("RegisterCallback", registerCallback);
    OH_VideoDecoder_Destroy(decoder);
    decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
    return false;
  }
  OH_AVFormat* format = OH_AVFormat_CreateVideoFormat(OH_AVCODEC_MIMETYPE_VIDEO_AVC, 1280, 720);
  if (format == nullptr) {
    fail("CreateVideoFormat", AV_ERR_NO_MEMORY);
    OH_VideoDecoder_Destroy(decoder);
    decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
    return false;
  }
  const OH_AVErrCode configure = OH_VideoDecoder_Configure(decoder, format);
  OH_AVFormat_Destroy(format);
  if (configure != AV_ERR_OK) {
    fail("Configure", configure);
    OH_VideoDecoder_Destroy(decoder);
    decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
    return false;
  }
  const OH_AVErrCode setSurface =
      OH_VideoDecoder_SetSurface(
          decoder,
          reinterpret_cast<OHNativeWindow*>(lifecycle_state_.surface()));
  if (setSurface != AV_ERR_OK) {
    fail("SetSurface", setSurface);
    OH_VideoDecoder_Destroy(decoder);
    decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
    return false;
  }
  const OH_AVErrCode prepare = OH_VideoDecoder_Prepare(decoder);
  if (prepare != AV_ERR_OK) {
    fail("Prepare", prepare);
    OH_VideoDecoder_Destroy(decoder);
    decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
    return false;
  }
  decoder_.store(decoder);
  decoder_callback_gate_.SetState(DecoderLifecycleState::kRunning);
  const OH_AVErrCode start = OH_VideoDecoder_Start(decoder);
  if (start != AV_ERR_OK) {
    fail("Start", start);
    decoder_callback_gate_.SetState(DecoderLifecycleState::kStopping);
    ClearDecoderQueues();
    decoder_.store(nullptr);
    OH_VideoDecoder_Destroy(decoder);
    decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
    return false;
  }
  decoder_recovery_.RequireCodecData();
  return true;
}

void ReceiverSession::ClearDecoderQueues() {
  std::scoped_lock queueLock(decoder_queue_mutex_);
  input_slots_.clear();
  decode_queue_.clear();
}

void ReceiverSession::DestroyDecoderLocked() {
  decoder_callback_gate_.SetState(DecoderLifecycleState::kStopping);
  ClearDecoderQueues();
  OH_AVCodec* decoder = decoder_.exchange(nullptr);
  if (decoder != nullptr) {
    // Never hold decoder_queue_mutex_ while lifecycle calls wait for callbacks.
    OH_VideoDecoder_Stop(decoder);
    OH_VideoDecoder_Destroy(decoder);
  }
  decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
  decoder_recovery_.RequireCodecData();
}

bool ReceiverSession::FlushDecoder() {
  bool recovered = false;
  {
    std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
    OH_AVCodec* decoder = decoder_.load();
    if (decoder != nullptr) {
      decoder_callback_gate_.BeginFlush([this] { ClearDecoderQueues(); });
      const bool flushed = OH_VideoDecoder_Flush(decoder) == AV_ERR_OK;
      bool restarted = false;
      if (flushed) {
        decoder_callback_gate_.SetState(DecoderLifecycleState::kRunning);
        restarted = OH_VideoDecoder_Start(decoder) == AV_ERR_OK;
      }
      if (EvaluateFlushRecovery(flushed, restarted) == FlushRecoveryAction::kResume) {
        recovered = true;
      } else {
        DestroyDecoderLocked();
        recovered =
            lifecycle_state_.DecoderShouldRun() && CreateDecoderLocked();
      }
    } else {
      decoder_callback_gate_.BeginFlush([this] { ClearDecoderQueues(); });
      decoder_callback_gate_.SetState(DecoderLifecycleState::kStopped);
    }
    decoder_recovery_.RequireCodecData();
  }
  ++decoder_resync_events_;
  RequestKeyframe();
  return recovered;
}

void ReceiverSession::SubmitFrame(DecodedInput frame) {
  std::scoped_lock queueLock(decoder_queue_mutex_);
  OH_AVCodec* decoder = decoder_.load();
  if (!decoder_callback_gate_.CallbacksAllowed() || decoder == nullptr ||
      !decoder_recovery_.InputAllowed(frame.kind)) {
    ++frames_dropped_;
    return;
  }
  const auto queuedFrames = decode_queue_.size();
  const auto admission = decoder_recovery_.Admit(
      queuedFrames, 3, [this] { decode_queue_.clear(); });
  if (!admission.accepted) {
    frames_dropped_ += queuedFrames;
    ++frames_dropped_;
    ++decoder_resync_events_;
    return;
  }
  decode_queue_.push_back(std::move(frame));
  PumpDecoderLocked(decoder);
}

bool ReceiverSession::SubmitRecovery(DecodedInput codecData, DecodedInput syncFrame) {
  std::scoped_lock queueLock(decoder_queue_mutex_);
  OH_AVCodec* decoder = decoder_.load();
  if (!decoder_callback_gate_.CallbacksAllowed() || decoder == nullptr ||
      decoder_recovery_.state() != DecoderRecoveryState::kNeedsCodecData ||
      codecData.kind != DecoderInputKind::kCodecData ||
      syncFrame.kind != DecoderInputKind::kSyncFrame || codecData.bytes.empty() ||
      syncFrame.bytes.empty()) {
    ++frames_dropped_;
    return false;
  }
  frames_dropped_ += decode_queue_.size();
  decode_queue_.clear();
  decode_queue_.push_back(std::move(codecData));
  decode_queue_.push_back(std::move(syncFrame));
  PumpDecoderLocked(decoder);
  return true;
}

void ReceiverSession::PumpDecoderLocked(OH_AVCodec* decoder) {
  while (decoder_callback_gate_.CallbacksAllowed() && decoder_.load() == decoder &&
         !input_slots_.empty() && !decode_queue_.empty()) {
    InputSlot slot = input_slots_.front();
    input_slots_.pop_front();
    DecodedInput frame = std::move(decode_queue_.front());
    decode_queue_.pop_front();
    if (!decoder_recovery_.InputAllowed(frame.kind)) {
      ++frames_dropped_;
      continue;
    }
    const int32_t capacity = OH_AVBuffer_GetCapacity(slot.buffer);
    auto* target = OH_AVBuffer_GetAddr(slot.buffer);
    if (capacity < 0 || target == nullptr || frame.bytes.size() > static_cast<std::size_t>(capacity)) {
      ++frames_dropped_;
      decoder_recovery_.RequireCodecData();
      frames_dropped_ += decode_queue_.size();
      decode_queue_.clear();
      ++decoder_resync_events_;
      RequestKeyframe();
      break;
    }
    std::memcpy(target, frame.bytes.data(), frame.bytes.size());
    OH_AVCodecBufferAttr attributes{};
    attributes.pts = static_cast<int64_t>(frame.timestampUs);
    attributes.size = static_cast<int32_t>(frame.bytes.size());
    attributes.offset = 0;
    attributes.flags = frame.kind == DecoderInputKind::kCodecData
                           ? AVCODEC_BUFFER_FLAGS_CODEC_DATA
                           : frame.kind == DecoderInputKind::kSyncFrame
                                 ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME
                                 : AVCODEC_BUFFER_FLAGS_NONE;
    const bool pushed = OH_AVBuffer_SetBufferAttr(slot.buffer, &attributes) == AV_ERR_OK &&
                        OH_VideoDecoder_PushInputBuffer(decoder, slot.index) == AV_ERR_OK;
    decoder_recovery_.OnInputSubmitted(frame.kind, pushed);
    if (!pushed) {
      ++frames_dropped_;
      frames_dropped_ += decode_queue_.size();
      decode_queue_.clear();
      ++decoder_resync_events_;
      RequestKeyframe();
      break;
    }
  }
}

void ReceiverSession::DecoderError(int32_t errorCode) {
  const bool connected = Status().connected;
  SetState("error", "视频播放出错（错误码 " + std::to_string(errorCode) + "）",
           connected, connected);
}

void ReceiverSession::DecoderNeedInput(OH_AVCodec* callbackDecoder, std::uint32_t index,
                                       OH_AVBuffer* buffer) {
  if (!decoder_callback_gate_.CallbacksAllowed()) return;
  std::scoped_lock queueLock(decoder_queue_mutex_);
  OH_AVCodec* decoder = decoder_.load();
  if (!decoder_callback_gate_.CallbacksAllowed() || decoder == nullptr ||
      decoder != callbackDecoder) {
    return;
  }
  input_slots_.push_back({index, buffer});
  PumpDecoderLocked(decoder);
}

void ReceiverSession::DecoderOutput(OH_AVCodec* decoder, std::uint32_t index,
                                    OH_AVBuffer* buffer) {
  if (!decoder_callback_gate_.CallbacksAllowed() || decoder_.load() != decoder) return;
  OH_AVCodecBufferAttr attributes{};
  if (OH_AVBuffer_GetBufferAttr(buffer, &attributes) == AV_ERR_OK &&
      (attributes.flags & AVCODEC_BUFFER_FLAGS_EOS) == 0) {
    if (OH_VideoDecoder_RenderOutputBuffer(decoder, index) == AV_ERR_OK) {
      const auto decoded = ++frames_decoded_;
      if (decoded == 1) {
        SetState("displaying", "正在播放电脑发送的画面", true, true);
      }
      std::ostringstream telemetry;
      telemetry << "{\"type\":\"telemetry\",\"protocol\":4,\"captureUs\":" << attributes.pts
                << ",\"displayUs\":" << ClockMicroseconds()
                << ",\"receivedFrames\":" << received_frames_.load()
                << ",\"receivedBytes\":" << received_bytes_.load()
                << ",\"receiverDecodedFrames\":" << decoded
                << ",\"receiverDroppedFrames\":" << frames_dropped_.load()
                << ",\"receiverResyncEvents\":" << decoder_resync_events_.load()
                << ",\"receiverKeyframeRequests\":" << keyframe_requests_sent_.load()
                << "}";
      telemetry_queue_.Push(telemetry.str());
      return;
    }
  }
  if (decoder_callback_gate_.CallbacksAllowed() && decoder_.load() == decoder) {
    OH_VideoDecoder_FreeOutputBuffer(decoder, index);
  }
}

void ReceiverSession::OnCodecError(OH_AVCodec*, int32_t errorCode, void* userData) {
  static_cast<ReceiverSession*>(userData)->DecoderError(errorCode);
}

void ReceiverSession::OnCodecNeedInput(OH_AVCodec* decoder, std::uint32_t index,
                                       OH_AVBuffer* buffer, void* userData) {
  static_cast<ReceiverSession*>(userData)->DecoderNeedInput(decoder, index, buffer);
}

void ReceiverSession::OnCodecOutput(OH_AVCodec* decoder, std::uint32_t index,
                                    OH_AVBuffer* buffer, void* userData) {
  static_cast<ReceiverSession*>(userData)->DecoderOutput(decoder, index, buffer);
}

void ReceiverSession::OnSurfaceCreated(OH_NativeXComponent* component, void* window) {
  bool decoderStarted = false;
  {
    std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
    decoderStarted = ApplyLifecycleDecisionLocked(
        lifecycle_state_.SurfaceCreated(
            reinterpret_cast<std::uintptr_t>(component),
            reinterpret_cast<std::uintptr_t>(window),
            DecoderRuntimeLocked()));
  }
  if (decoderStarted) RequestKeyframe();
}

void ReceiverSession::OnSurfaceChanged(OH_NativeXComponent* component, void* window) {
  bool decoderStarted = false;
  {
    std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
    decoderStarted = ApplyLifecycleDecisionLocked(
        lifecycle_state_.SurfaceChanged(
            reinterpret_cast<std::uintptr_t>(component),
            reinterpret_cast<std::uintptr_t>(window),
            DecoderRuntimeLocked()));
  }
  if (decoderStarted) RequestKeyframe();
}

void ReceiverSession::OnSurfaceDestroyed(OH_NativeXComponent* component, void* window) {
  std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
  ApplyLifecycleDecisionLocked(
      lifecycle_state_.SurfaceDestroyed(
          reinterpret_cast<std::uintptr_t>(component),
          reinterpret_cast<std::uintptr_t>(window),
          DecoderRuntimeLocked()));
}

void ReceiverSession::OnAppForeground() {
  bool decoderStarted = false;
  {
    std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
    decoderStarted = ApplyLifecycleDecisionLocked(
        lifecycle_state_.Foreground(DecoderRuntimeLocked()));
  }
  if (decoderStarted) RequestKeyframe();
}

void ReceiverSession::OnAppBackground() {
  std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
  ApplyLifecycleDecisionLocked(
      lifecycle_state_.Background(DecoderRuntimeLocked()));
}

}  // namespace hss::receiver
