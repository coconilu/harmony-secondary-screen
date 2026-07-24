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
#include <climits>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace hss::receiver {
namespace {

constexpr std::uint16_t kControlPort = 44000;
constexpr std::uint16_t kVideoPort = 47101;
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

bool IsCurrentWifiIpv4(const std::string& value, in_addr* address) {
  if (!ValidIpv4(value, address)) return false;
  const std::uint32_t hostOrder = ntohl(address->s_addr);
  if (hostOrder == 0 || (hostOrder >> 24U) == 127U || (hostOrder >> 28U) == 14U) {
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
      matched = true;
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

std::string RandomPairingCode() {
  std::uint32_t value = 0;
  constexpr std::uint32_t kRange = 1'000'000U;
  constexpr std::uint32_t kLimit = UINT32_MAX - (UINT32_MAX % kRange);
  do {
    if (!RandomBytes(&value, sizeof(value))) return {};
  } while (value >= kLimit);
  std::ostringstream output;
  output << std::setw(6) << std::setfill('0') << value % kRange;
  return output.str();
}

std::uint32_t RandomSessionShort() {
  std::uint32_t value = 0;
  while (value == 0 && RandomBytes(&value, sizeof(value))) {
  }
  return value;
}

std::string Ipv4Text(const in_addr& address) {
  std::array<char, INET_ADDRSTRLEN> text{};
  return inet_ntop(AF_INET, &address, text.data(), text.size()) == nullptr ? "" : text.data();
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
  if (!IsCurrentWifiIpv4(listenAddress, &address)) {
    SetState("error", "该地址不是本机当前启用的 Wi-Fi IPv4", false, false);
    return false;
  }
  Stop();
  const std::string pairingCode = RandomPairingCode();
  const std::string receiverNonce = RandomHex(16);
  if (pairingCode.empty() || receiverNonce.empty()) {
    SetState("error", "无法生成安全配对凭据", false, false);
    return false;
  }
  {
    std::scoped_lock lock(state_mutex_);
    listen_address_ = std::move(listenAddress);
    paired_address_.clear();
    pairing_code_ = pairingCode;
    receiver_nonce_ = receiverNonce;
    session_id_.clear();
    session_short_ = 0;
    pairing_expires_at_ = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  }
  frames_decoded_ = 0;
  frames_dropped_ = 0;
  desired_ = true;
  SetState("starting", "正在绑定已确认的 Wi-Fi 地址", false, false);
  worker_ = std::thread(&ReceiverSession::NetworkLoop, this);
  return true;
}

void ReceiverSession::Stop() {
  desired_ = false;
  CloseSockets();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
    worker_.join();
  }
  assemblies_.clear();
  control_decoder_.Reset();
  telemetry_queue_.Clear();
  keyframe_request_pending_ = false;
  SetState("idle", "请输入本机 Wi-Fi IPv4", false, false);
}

StatusSnapshot ReceiverSession::Status() const {
  std::scoped_lock lock(state_mutex_);
  return {state_, detail_, listen_address_, pairing_code_, paired_address_, listening_,
          connected_, frames_decoded_.load(), frames_dropped_.load()};
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
  if (!OpenListeners()) {
    if (desired_) SetState("error", "无法在该 Wi-Fi 地址绑定 44000/47101", false, false);
    CloseSockets();
    return;
  }
  SetState("listening", "等待 Windows 发送端输入配对码", true, false);
  while (desired_ && !AcceptAndPair()) {
  }
  if (desired_) {
    RunConnectedSession();
  }
  CloseSockets();
  assemblies_.clear();
  control_decoder_.Reset();
  telemetry_queue_.Clear();
  keyframe_request_pending_ = false;
  if (desired_) {
    desired_ = false;
    SetState("stopped", "会话已结束；请重新开始接收以生成新配对码", false, false);
  }
}

bool ReceiverSession::OpenListeners() {
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

  const int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp < 0) return false;
  video_socket_ = udp;
  setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in udpAddress{};
  udpAddress.sin_family = AF_INET;
  udpAddress.sin_addr = address;
  udpAddress.sin_port = htons(kVideoPort);
  if (bind(udp, reinterpret_cast<sockaddr*>(&udpAddress), sizeof(udpAddress)) != 0) {
    return false;
  }
  return true;
}

bool ReceiverSession::AcceptAndPair() {
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
  control_socket_ = client;
  control_decoder_.Reset();

  bool helloSeen = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::array<std::byte, 8192> buffer{};
  while (desired_ && std::chrono::steady_clock::now() < deadline) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(client, &readSet);
    timeval readTimeout{0, 100'000};
    const int readable = select(client + 1, &readSet, nullptr, nullptr, &readTimeout);
    if (readable < 0 && errno != EINTR) break;
    if (readable <= 0) continue;
    const ssize_t count = recv(client, buffer.data(), buffer.size(), 0);
    if (count <= 0) break;
    std::vector<std::string> frames;
    if (!control_decoder_.Push(buffer.data(), static_cast<std::size_t>(count), &frames)) break;
    for (const auto& json : frames) {
      const auto type = protocol::JsonString(json, "type");
      const auto version = protocol::JsonInteger(json, "protocol");
      if (version != protocol::kVersion) {
        SendControl(R"({"type":"error","code":"protocol_mismatch"})");
        CloseControlSocket();
        return false;
      }
      if (type == "hello") {
        std::string nonce;
        std::int64_t expires = 0;
        {
          std::scoped_lock lock(state_mutex_);
          nonce = receiver_nonce_;
          expires = std::max<std::int64_t>(
              0, std::chrono::duration_cast<std::chrono::seconds>(
                     pairing_expires_at_ - std::chrono::steady_clock::now()).count());
        }
        std::ostringstream reply;
        reply << "{\"type\":\"hello\",\"protocol\":2,\"receiverNonce\":\""
              << protocol::EscapeJson(nonce) << "\",\"pairingExpiresInSec\":" << expires << "}";
        if (!SendControl(reply.str())) {
          CloseControlSocket();
          return false;
        }
        helloSeen = true;
        continue;
      }
      if (type != "pair" || !helloSeen) {
        CloseControlSocket();
        return false;
      }

      std::string expectedCode;
      std::string expectedNonce;
      std::chrono::steady_clock::time_point expiry;
      {
        std::scoped_lock lock(state_mutex_);
        expectedCode = pairing_code_;
        expectedNonce = receiver_nonce_;
        expiry = pairing_expires_at_;
      }
      const bool pairingValid =
          std::chrono::steady_clock::now() < expiry &&
          protocol::JsonString(json, "pairingCode") == expectedCode &&
          protocol::JsonString(json, "receiverNonce") == expectedNonce &&
          protocol::JsonString(json, "senderNonce").value_or("").size() >= 16;
      if (!pairingValid) {
        SendControl(R"({"type":"error","code":"pairing_failed"})");
        CloseControlSocket();
        return false;
      }
      const bool codecValid =
          protocol::JsonString(json, "codec") == "video/avc" &&
          protocol::JsonString(json, "avcFormat") == "annexb" &&
          protocol::JsonInteger(json, "width") == 1280 &&
          protocol::JsonInteger(json, "height") == 720 &&
          protocol::JsonInteger(json, "fps") == 30;
      if (!codecValid) {
        SendControl(R"({"type":"error","code":"codec_unsupported"})");
        CloseControlSocket();
        return false;
      }
      const std::string sessionId = RandomHex(16);
      const std::uint32_t sessionShort = RandomSessionShort();
      if (sessionId.empty() || sessionShort == 0) {
        CloseControlSocket();
        return false;
      }
      {
        std::scoped_lock lock(state_mutex_);
        session_id_ = sessionId;
        session_short_ = sessionShort;
        pairing_code_.clear();
        paired_address_ = Ipv4Text(peer.sin_addr);
      }
      std::ostringstream reply;
      reply << "{\"type\":\"session\",\"protocol\":2,\"sessionId\":\""
            << sessionId << "\",\"sessionShort\":" << sessionShort
            << ",\"codec\":\"video/avc\",\"avcFormat\":\"annexb\","
               "\"width\":1280,\"height\":720,\"fps\":30,\"videoPort\":47101}";
      if (!SendControl(reply.str())) {
        CloseControlSocket();
        return false;
      }
      SetState("connected", "已配对，等待 H.264 关键帧", true, true);
      keyframe_request_pending_ = true;
      return true;
    }
  }
  CloseControlSocket();
  return false;
}

bool ReceiverSession::RunConnectedSession() {
  int tcp = control_socket_.load();
  int udp = video_socket_.load();
  if (tcp < 0 || udp < 0) return false;
  std::array<std::byte, protocol::kHeaderSize + protocol::kMaxUdpPayload> udpBuffer{};
  std::array<std::byte, 8192> tcpBuffer{};
  while (desired_) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(tcp, &readSet);
    FD_SET(udp, &readSet);
    timeval timeout{0, 100000};
    const int ready = select(std::max(tcp, udp) + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready < 0 && errno != EINTR) return false;
    if (ready > 0 && FD_ISSET(tcp, &readSet)) {
      const ssize_t count = recv(tcp, tcpBuffer.data(), tcpBuffer.size(), 0);
      if (count <= 0) return false;
      std::vector<std::string> frames;
      if (!control_decoder_.Push(tcpBuffer.data(), static_cast<std::size_t>(count), &frames)) return false;
      for (const auto& json : frames) {
        if (!HandleControl(json)) return false;
      }
    }
    if (ready > 0 && FD_ISSET(udp, &readSet)) {
      sockaddr_in source{};
      socklen_t sourceSize = sizeof(source);
      const ssize_t count = recvfrom(udp, udpBuffer.data(), udpBuffer.size(), 0,
                                     reinterpret_cast<sockaddr*>(&source), &sourceSize);
      std::string pairedAddress;
      {
        std::scoped_lock lock(state_mutex_);
        pairedAddress = paired_address_;
      }
      if (count > 0 && Ipv4Text(source.sin_addr) == pairedAddress) {
        HandleVideo(udpBuffer.data(), static_cast<std::size_t>(count));
      }
    }
    if (keyframe_request_pending_.exchange(false) &&
        !SendControl(R"({"type":"keyframe","reason":"loss_flush_or_session_start","requireCodecConfig":true})")) {
      return false;
    }
    std::string telemetry;
    if (telemetry_queue_.TryPop(&telemetry) && !SendControl(telemetry)) return false;
    SweepAssemblies();
  }
  return false;
}

bool ReceiverSession::SendControl(std::string_view json) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  std::unique_lock<std::timed_mutex> lock(send_mutex_, std::defer_lock);
  if (!lock.try_lock_until(deadline)) return false;
  const int descriptor = control_socket_.load();
  if (descriptor < 0) return false;
  const auto frame = protocol::EncodeControl(json);
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
  const auto type = protocol::JsonString(json, "type");
  if (type == "ping") {
    const auto senderSend = protocol::JsonInteger(json, "senderSendUs");
    if (!senderSend) return false;
    const auto receiveUs = ClockMicroseconds();
    std::ostringstream pong;
    pong << "{\"type\":\"pong\",\"senderSendUs\":" << *senderSend
         << ",\"receiverReceiveUs\":" << receiveUs
         << ",\"receiverSendUs\":" << ClockMicroseconds() << "}";
    return SendControl(pong.str());
  }
  return type != "stop";
}

void ReceiverSession::HandleVideo(const std::byte* data, std::size_t size) {
  const auto header = protocol::DecodeVideoHeader(data, size);
  std::uint32_t expectedSession = 0;
  {
    std::scoped_lock lock(state_mutex_);
    expectedSession = session_short_;
  }
  if (!header || header->session != expectedSession) return;
  if (assemblies_.size() >= 4 && !assemblies_.contains(header->frame)) {
    assemblies_.erase(assemblies_.begin());
    ++frames_dropped_;
  }
  auto [iterator, inserted] = assemblies_.try_emplace(header->frame);
  Assembly& assembly = iterator->second;
  if (inserted) {
    assembly.fragmentCount = header->fragments;
    assembly.flags = header->flags & ~protocol::kEndOfFrame;
    assembly.timestampUs = header->timestampUs;
    assembly.created = std::chrono::steady_clock::now();
    assembly.fragments.resize(header->fragments);
    assembly.received.resize(header->fragments, false);
  }
  const bool finalFragment = header->fragment + 1U == header->fragments;
  const bool hasEndFlag = (header->flags & protocol::kEndOfFrame) != 0;
  if (assembly.fragmentCount != header->fragments ||
      assembly.flags != (header->flags & ~protocol::kEndOfFrame) ||
      assembly.timestampUs != header->timestampUs || assembly.received[header->fragment]) {
    ++frames_dropped_;
    assemblies_.erase(iterator);
    RequestKeyframe();
    return;
  }
  if (finalFragment != hasEndFlag) {
    ++frames_dropped_;
    assemblies_.erase(iterator);
    RequestKeyframe();
    return;
  }
  assembly.flags |= header->flags & protocol::kEndOfFrame;
  const auto* payload = data + protocol::kHeaderSize;
  assembly.fragments[header->fragment].assign(payload, payload + header->payloadLength);
  assembly.received[header->fragment] = true;
  ++assembly.receivedCount;
  if (assembly.receivedCount != assembly.fragmentCount) return;

  const auto recoveryState = decoder_recovery_state_.load();
  const bool keyframe = (assembly.flags & protocol::kKeyframe) != 0;
  const bool codecConfig = (assembly.flags & protocol::kCodecConfig) != 0;
  if (recoveryState == DecoderRecoveryState::kNeedsSyncFrame) {
    ++frames_dropped_;
    assemblies_.erase(iterator);
    return;
  }
  if (recoveryState == DecoderRecoveryState::kNeedsCodecData &&
      (assembly.flags & (protocol::kKeyframe | protocol::kCodecConfig)) !=
          (protocol::kKeyframe | protocol::kCodecConfig)) {
    ++frames_dropped_;
    assemblies_.erase(iterator);
    RequestKeyframe();
    return;
  }
  std::size_t total = 0;
  for (const auto& fragment : assembly.fragments) total += fragment.size();
  if (total == 0 || total > protocol::kMaxFrameBytes ||
      (assembly.flags & protocol::kEndOfFrame) == 0) {
    ++frames_dropped_;
  } else {
    std::vector<std::byte> bytes;
    bytes.reserve(total);
    for (const auto& fragment : assembly.fragments) {
      bytes.insert(bytes.end(), fragment.begin(), fragment.end());
    }
    if (recoveryState == DecoderRecoveryState::kNeedsCodecData) {
      auto recovery = SplitAvcRecoveryInput(bytes);
      if (!keyframe || !codecConfig || !recovery.complete()) {
        ++frames_dropped_;
        RequestKeyframe();
      } else {
        DecodedInput configInput{std::move(recovery.codecData), assembly.timestampUs,
                                 DecoderInputKind::kCodecData};
        DecodedInput syncInput{std::move(recovery.syncFrame), assembly.timestampUs,
                               DecoderInputKind::kSyncFrame};
        if (!SubmitRecovery(std::move(configInput), std::move(syncInput))) {
          RequestKeyframe();
        }
      }
    } else {
      DecodedInput frame{std::move(bytes), assembly.timestampUs,
                         keyframe ? DecoderInputKind::kSyncFrame
                                  : DecoderInputKind::kFrame};
      SubmitFrame(std::move(frame));
    }
  }
  assemblies_.erase(iterator);
}

void ReceiverSession::SweepAssemblies() {
  const auto deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(150);
  bool lost = false;
  for (auto iterator = assemblies_.begin(); iterator != assemblies_.end();) {
    if (iterator->second.created < deadline) {
      iterator = assemblies_.erase(iterator);
      ++frames_dropped_;
      lost = true;
    } else {
      ++iterator;
    }
  }
  if (lost) RequestKeyframe();
}

void ReceiverSession::RequestKeyframe() {
  keyframe_request_pending_ = true;
}

void ReceiverSession::CloseControlSocket() {
  CloseSocket(&control_socket_);
}

void ReceiverSession::CloseSockets() {
  CloseSocket(&control_socket_);
  CloseSocket(&video_socket_);
  CloseSocket(&listener_socket_);
}

bool ReceiverSession::StartDecoder() {
  std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
  DestroyDecoderLocked();
  return CreateDecoderLocked();
}

bool ReceiverSession::CreateDecoderLocked() {
  const bool connected = Status().connected;
  const auto fail = [this, connected](const char* operation, int32_t errorCode) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, kLogTag,
                 "AVCodec initialization failed at %{public}s, error=%{public}d",
                 operation, errorCode);
    SetState("error",
             std::string("解码器初始化失败：") + operation + "（错误码 " +
                 std::to_string(errorCode) + "）",
             connected, connected);
  };

  if (native_window_ == nullptr) {
    fail("Surface", AV_ERR_INVALID_VAL);
    return false;
  }
  decoder_state_ = DecoderLifecycleState::kStarting;
  OH_AVCodec* decoder = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
  if (decoder == nullptr) {
    fail("CreateByMime", AV_ERR_UNSUPPORT);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  OH_AVCodecCallback callbacks{OnCodecError, OnCodecStreamChanged,
                                OnCodecNeedInput, OnCodecOutput};
  const OH_AVErrCode registerCallback = OH_VideoDecoder_RegisterCallback(decoder, callbacks, this);
  if (registerCallback != AV_ERR_OK) {
    fail("RegisterCallback", registerCallback);
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  OH_AVFormat* format = OH_AVFormat_CreateVideoFormat(OH_AVCODEC_MIMETYPE_VIDEO_AVC, 1280, 720);
  if (format == nullptr) {
    fail("CreateVideoFormat", AV_ERR_NO_MEMORY);
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  const OH_AVErrCode configure = OH_VideoDecoder_Configure(decoder, format);
  OH_AVFormat_Destroy(format);
  if (configure != AV_ERR_OK) {
    fail("Configure", configure);
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  const OH_AVErrCode setSurface =
      OH_VideoDecoder_SetSurface(decoder, static_cast<OHNativeWindow*>(native_window_));
  if (setSurface != AV_ERR_OK) {
    fail("SetSurface", setSurface);
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  const OH_AVErrCode prepare = OH_VideoDecoder_Prepare(decoder);
  if (prepare != AV_ERR_OK) {
    fail("Prepare", prepare);
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  decoder_.store(decoder);
  decoder_state_ = DecoderLifecycleState::kRunning;
  const OH_AVErrCode start = OH_VideoDecoder_Start(decoder);
  if (start != AV_ERR_OK) {
    fail("Start", start);
    decoder_state_ = DecoderLifecycleState::kStopping;
    ClearDecoderQueues();
    decoder_.store(nullptr);
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  decoder_recovery_state_ = DecoderRecoveryState::kNeedsCodecData;
  return true;
}

void ReceiverSession::ClearDecoderQueues() {
  std::scoped_lock queueLock(decoder_queue_mutex_);
  input_slots_.clear();
  decode_queue_.clear();
}

void ReceiverSession::DestroyDecoderLocked() {
  decoder_state_ = DecoderLifecycleState::kStopping;
  ClearDecoderQueues();
  OH_AVCodec* decoder = decoder_.exchange(nullptr);
  if (decoder != nullptr) {
    // Never hold decoder_queue_mutex_ while lifecycle calls wait for callbacks.
    OH_VideoDecoder_Stop(decoder);
    OH_VideoDecoder_Destroy(decoder);
  }
  decoder_state_ = DecoderLifecycleState::kStopped;
  decoder_recovery_state_ = DecoderRecoveryState::kNeedsCodecData;
}

void ReceiverSession::StopDecoder() {
  std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
  DestroyDecoderLocked();
}

bool ReceiverSession::FlushDecoder() {
  bool recovered = false;
  {
    std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
    OH_AVCodec* decoder = decoder_.load();
    if (decoder == nullptr) return false;
    decoder_state_ = DecoderLifecycleState::kFlushing;
    ClearDecoderQueues();
    const bool flushed = OH_VideoDecoder_Flush(decoder) == AV_ERR_OK;
    bool restarted = false;
    if (flushed) {
      decoder_state_ = DecoderLifecycleState::kRunning;
      restarted = OH_VideoDecoder_Start(decoder) == AV_ERR_OK;
    }
    if (EvaluateFlushRecovery(flushed, restarted) == FlushRecoveryAction::kResume) {
      recovered = true;
    } else {
      DestroyDecoderLocked();
      recovered = CreateDecoderLocked();
    }
    decoder_recovery_state_ = DecoderRecoveryState::kNeedsCodecData;
  }
  if (recovered) RequestKeyframe();
  return recovered;
}

void ReceiverSession::SubmitFrame(DecodedInput frame) {
  std::scoped_lock queueLock(decoder_queue_mutex_);
  OH_AVCodec* decoder = decoder_.load();
  if (!DecoderCallbacksAllowed(decoder_state_.load()) || decoder == nullptr ||
      !DecoderInputAllowed(decoder_recovery_state_.load(), frame.kind)) {
    ++frames_dropped_;
    return;
  }
  while (decode_queue_.size() >= 3) {
    decode_queue_.pop_front();
    ++frames_dropped_;
  }
  decode_queue_.push_back(std::move(frame));
  PumpDecoderLocked(decoder);
}

bool ReceiverSession::SubmitRecovery(DecodedInput codecData, DecodedInput syncFrame) {
  std::scoped_lock queueLock(decoder_queue_mutex_);
  OH_AVCodec* decoder = decoder_.load();
  if (!DecoderCallbacksAllowed(decoder_state_.load()) || decoder == nullptr ||
      decoder_recovery_state_.load() != DecoderRecoveryState::kNeedsCodecData ||
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
  while (DecoderCallbacksAllowed(decoder_state_.load()) && decoder_.load() == decoder &&
         !input_slots_.empty() && !decode_queue_.empty()) {
    InputSlot slot = input_slots_.front();
    input_slots_.pop_front();
    DecodedInput frame = std::move(decode_queue_.front());
    decode_queue_.pop_front();
    const auto recoveryState = decoder_recovery_state_.load();
    if (!DecoderInputAllowed(recoveryState, frame.kind)) {
      ++frames_dropped_;
      continue;
    }
    const int32_t capacity = OH_AVBuffer_GetCapacity(slot.buffer);
    auto* target = OH_AVBuffer_GetAddr(slot.buffer);
    if (capacity < 0 || target == nullptr || frame.bytes.size() > static_cast<std::size_t>(capacity)) {
      ++frames_dropped_;
      decoder_recovery_state_ = DecoderRecoveryState::kNeedsCodecData;
      frames_dropped_ += decode_queue_.size();
      decode_queue_.clear();
      keyframe_request_pending_ = true;
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
    decoder_recovery_state_ = AdvanceDecoderRecovery(recoveryState, frame.kind, pushed);
    if (!pushed) {
      ++frames_dropped_;
      frames_dropped_ += decode_queue_.size();
      decode_queue_.clear();
      keyframe_request_pending_ = true;
      break;
    }
  }
}

void ReceiverSession::DecoderError(int32_t errorCode) {
  const bool connected = Status().connected;
  SetState("error", "AVCodec 解码错误: " + std::to_string(errorCode), connected, connected);
}

void ReceiverSession::DecoderNeedInput(OH_AVCodec* callbackDecoder, std::uint32_t index,
                                       OH_AVBuffer* buffer) {
  if (!DecoderCallbacksAllowed(decoder_state_.load())) return;
  std::scoped_lock queueLock(decoder_queue_mutex_);
  OH_AVCodec* decoder = decoder_.load();
  if (!DecoderCallbacksAllowed(decoder_state_.load()) || decoder == nullptr ||
      decoder != callbackDecoder) {
    return;
  }
  input_slots_.push_back({index, buffer});
  PumpDecoderLocked(decoder);
}

void ReceiverSession::DecoderOutput(OH_AVCodec* decoder, std::uint32_t index,
                                    OH_AVBuffer* buffer) {
  if (!DecoderCallbacksAllowed(decoder_state_.load()) || decoder_.load() != decoder) return;
  OH_AVCodecBufferAttr attributes{};
  if (OH_AVBuffer_GetBufferAttr(buffer, &attributes) == AV_ERR_OK &&
      (attributes.flags & AVCODEC_BUFFER_FLAGS_EOS) == 0) {
    if (OH_VideoDecoder_RenderOutputBuffer(decoder, index) == AV_ERR_OK) {
      const auto decoded = ++frames_decoded_;
      if (decoded == 1) {
        SetState("displaying", "首个 H.264 关键帧已由 AVCodec 显示", true, true);
      }
      std::ostringstream telemetry;
      telemetry << "{\"type\":\"telemetry\",\"captureUs\":" << attributes.pts
                << ",\"displayUs\":" << ClockMicroseconds()
                << ",\"framesDecoded\":" << decoded
                << ",\"framesDropped\":" << frames_dropped_.load() << "}";
      telemetry_queue_.Push(telemetry.str());
      return;
    }
  }
  if (DecoderCallbacksAllowed(decoder_state_.load()) && decoder_.load() == decoder) {
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

void ReceiverSession::OnSurfaceCreated(OH_NativeXComponent*, void* window) {
  {
    std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
    native_window_ = window;
  }
  StartDecoder();
}

void ReceiverSession::OnSurfaceChanged(OH_NativeXComponent*, void* window) {
  {
    std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
    native_window_ = window;
  }
  StartDecoder();
  RequestKeyframe();
}

void ReceiverSession::OnSurfaceDestroyed() {
  std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
  DestroyDecoderLocked();
  native_window_ = nullptr;
}

}  // namespace hss::receiver
