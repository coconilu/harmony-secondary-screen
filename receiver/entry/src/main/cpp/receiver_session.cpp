#include "receiver_session.h"

#include "connect_policy.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sstream>

namespace hss::receiver {
namespace {

constexpr std::uint16_t kControlPort = 47100;
constexpr std::uint16_t kVideoPort = 47101;
constexpr std::size_t kMaxFrameBytes = 16U * 1024U * 1024U;

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

}  // namespace

ReceiverSession& ReceiverSession::Instance() {
  static ReceiverSession session;
  return session;
}

ReceiverSession::~ReceiverSession() {
  Stop();
}

bool ReceiverSession::Start(std::string host, std::string pairingCode) {
  in_addr address{};
  if (!ValidIpv4(host, &address) || pairingCode.size() != 6 ||
      !std::all_of(pairingCode.begin(), pairingCode.end(), [](char value) { return value >= '0' && value <= '9'; })) {
    SetState("error", "主机 IP 或六位配对码无效", false);
    return false;
  }
  Stop();
  {
    std::scoped_lock lock(state_mutex_);
    host_ = std::move(host);
    pairing_code_ = std::move(pairingCode);
    session_id_.clear();
    session_short_ = 0;
  }
  desired_ = true;
  SetState("connecting", "正在连接 Windows Host", false);
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
  FlushDecoder();
  SetState("idle", "等待连接", false);
}

StatusSnapshot ReceiverSession::Status() const {
  std::scoped_lock lock(state_mutex_);
  return {state_, detail_, connected_, frames_decoded_.load(), frames_dropped_.load()};
}

void ReceiverSession::SetState(std::string state, std::string detail, bool connected) {
  std::scoped_lock lock(state_mutex_);
  state_ = std::move(state);
  detail_ = std::move(detail);
  connected_ = connected;
}

void ReceiverSession::NetworkLoop() {
  const std::array<int, 5> retryDelayMs{200, 400, 800, 1000, 1000};
  std::size_t retry = 0;
  while (desired_) {
    bool resume = false;
    {
      std::scoped_lock lock(state_mutex_);
      resume = !session_id_.empty();
    }
    SetState(resume ? "reconnecting" : "connecting",
             resume ? "连接中断，正在自动恢复" : "正在进行一次性配对", false);
    if (ConnectControl(resume) && RunConnectedSession()) {
      retry = 0;
    }
    CloseSockets();
    assemblies_.clear();
    control_decoder_.Reset();
    if (desired_ && !FlushDecoder()) {
      SetState("warning", "AVCodec 恢复失败，等待 Surface 重建", false);
    }
    if (!desired_) break;
    const int delay = retryDelayMs[std::min(retry, retryDelayMs.size() - 1)];
    ++retry;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
}

bool ReceiverSession::ConnectControl(bool resume) {
  std::string host;
  std::string code;
  std::string sessionId;
  {
    std::scoped_lock lock(state_mutex_);
    host = host_;
    code = pairing_code_;
    sessionId = session_id_;
  }

  const int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp < 0) return false;
  sockaddr_in udpAddress{};
  udpAddress.sin_family = AF_INET;
  udpAddress.sin_addr.s_addr = htonl(INADDR_ANY);
  udpAddress.sin_port = htons(kVideoPort);
  int reuse = 1;
  setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (bind(udp, reinterpret_cast<sockaddr*>(&udpAddress), sizeof(udpAddress)) != 0) {
    close(udp);
    return false;
  }
  video_socket_ = udp;

  const int tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (tcp < 0) return false;
  control_socket_ = tcp;
  sockaddr_in server{};
  server.sin_family = AF_INET;
  server.sin_port = htons(kControlPort);
  if (!ValidIpv4(host, &server.sin_addr)) {
    CloseSocket(&control_socket_);
    return false;
  }
  const int originalFlags = fcntl(tcp, F_GETFL, 0);
  if (originalFlags < 0 || fcntl(tcp, F_SETFL, originalFlags | O_NONBLOCK) != 0) {
    CloseSocket(&control_socket_);
    return false;
  }
  bool connected = connect(tcp, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == 0;
  if (!connected && errno != EINPROGRESS) {
    CloseSocket(&control_socket_);
    return false;
  }
  const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!connected) {
    const bool expired = std::chrono::steady_clock::now() >= connectDeadline;
    const auto initialDecision = EvaluateConnectWait(desired_.load(), expired, false, 0);
    if (initialDecision == ConnectWaitDecision::kCancelled ||
        initialDecision == ConnectWaitDecision::kTimedOut) {
      CloseSocket(&control_socket_);
      return false;
    }
    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    FD_SET(tcp, &writeSet);
    FD_SET(tcp, &errorSet);
    timeval timeout{0, 50'000};
    const int ready = select(tcp + 1, nullptr, &writeSet, &errorSet, &timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      CloseSocket(&control_socket_);
      return false;
    }
    int socketError = 0;
    socklen_t errorSize = sizeof(socketError);
    const bool socketReady = ready > 0 &&
                             (FD_ISSET(tcp, &writeSet) || FD_ISSET(tcp, &errorSet));
    if (socketReady && getsockopt(tcp, SOL_SOCKET, SO_ERROR, &socketError, &errorSize) != 0) {
      socketError = errno == 0 ? EIO : errno;
    }
    const auto decision = EvaluateConnectWait(desired_.load(), false, socketReady, socketError);
    if (decision == ConnectWaitDecision::kConnected) {
      connected = true;
    } else if (decision == ConnectWaitDecision::kFailed ||
               decision == ConnectWaitDecision::kCancelled) {
      CloseSocket(&control_socket_);
      return false;
    }
  }
  if (!desired_ || fcntl(tcp, F_SETFL, originalFlags) != 0) {
    CloseSocket(&control_socket_);
    return false;
  }

  std::ostringstream auth;
  if (resume) {
    auth << "{\"type\":\"resume\",\"protocol\":1,\"sessionId\":\""
         << protocol::EscapeJson(sessionId) << "\",\"videoPort\":47101}";
  } else {
    auth << "{\"type\":\"pair\",\"protocol\":1,\"pairingCode\":\""
         << protocol::EscapeJson(code)
         << "\",\"receiverNonce\":\"" << std::hex << ClockMicroseconds()
         << "\",\"videoPort\":47101,\"codec\":\"video/avc\","
            "\"width\":1920,\"height\":1200,\"fps\":60}";
  }
  if (!SendControl(auth.str())) return false;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::array<std::byte, 8192> buffer{};
  while (desired_ && std::chrono::steady_clock::now() < deadline) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(tcp, &set);
    timeval timeout{0, 200000};
    if (select(tcp + 1, &set, nullptr, nullptr, &timeout) <= 0) continue;
    const ssize_t count = recv(tcp, buffer.data(), buffer.size(), 0);
    if (count <= 0) return false;
    std::vector<std::string> frames;
    if (!control_decoder_.Push(buffer.data(), static_cast<std::size_t>(count), &frames)) return false;
    for (const auto& json : frames) {
      const auto type = protocol::JsonString(json, "type");
      if (type == "error") return false;
      if (type == "session") {
        const auto receivedId = protocol::JsonString(json, "sessionId");
        const auto shortId = protocol::JsonInteger(json, "sessionShort");
        const auto width = protocol::JsonInteger(json, "width");
        const auto height = protocol::JsonInteger(json, "height");
        const auto fps = protocol::JsonInteger(json, "fps");
        if (!receivedId || receivedId->size() != 32 || !shortId || *shortId <= 0 ||
            width != 1920 || height != 1200 || fps != 60) {
          return false;
        }
        {
          std::scoped_lock lock(state_mutex_);
          session_id_ = *receivedId;
          session_short_ = static_cast<std::uint32_t>(*shortId);
        }
        SetState("connected", "1920×1200 @ 60 Hz · H.264", true);
        RequestKeyframe();
        return true;
      }
    }
  }
  return false;
}

bool ReceiverSession::RunConnectedSession() {
  int tcp = control_socket_.load();
  int udp = video_socket_.load();
  if (tcp < 0 || udp < 0) return false;
  auto nextHeartbeat = std::chrono::steady_clock::now();
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
      for (const auto& json : frames) HandleControl(json);
    }
    if (ready > 0 && FD_ISSET(udp, &readSet)) {
      const ssize_t count = recv(udp, udpBuffer.data(), udpBuffer.size(), 0);
      if (count > 0) HandleVideo(udpBuffer.data(), static_cast<std::size_t>(count));
    }
    if (std::chrono::steady_clock::now() >= nextHeartbeat) {
      if (!SendControl("{\"type\":\"ping\",\"clientSendUs\":" +
                       std::to_string(ClockMicroseconds()) + "}")) {
        return false;
      }
      nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    SweepAssemblies();
  }
  return false;
}

bool ReceiverSession::SendControl(std::string_view json) {
  std::scoped_lock lock(send_mutex_);
  const int descriptor = control_socket_.load();
  if (descriptor < 0) return false;
  const auto frame = protocol::EncodeControl(json);
  std::size_t sent = 0;
  while (sent < frame.size()) {
    const ssize_t count = send(descriptor, frame.data() + sent, frame.size() - sent, 0);
    if (count <= 0) return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

void ReceiverSession::HandleControl(std::string_view json) {
  const auto type = protocol::JsonString(json, "type");
  if (type == "pong") {
    const auto clientSend = protocol::JsonInteger(json, "clientSendUs");
    const auto serverReceive = protocol::JsonInteger(json, "serverReceiveUs");
    const auto serverSend = protocol::JsonInteger(json, "serverSendUs");
    const auto clientReceive = static_cast<std::int64_t>(ClockMicroseconds());
    if (clientSend && serverReceive && serverSend) {
      host_clock_offset_us_ = ((*serverReceive - *clientSend) +
                               (*serverSend - clientReceive)) / 2;
    }
  } else if (type == "error") {
    SetState("warning", protocol::JsonString(json, "message").value_or("Host 拒绝了控制消息"), true);
  }
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
    assembly.flags = header->flags;
    assembly.timestampUs = header->timestampUs;
    assembly.created = std::chrono::steady_clock::now();
    assembly.fragments.resize(header->fragments);
    assembly.received.resize(header->fragments, false);
  }
  if (assembly.fragmentCount != header->fragments || assembly.received[header->fragment]) return;
  const auto* payload = data + protocol::kHeaderSize;
  assembly.fragments[header->fragment].assign(payload, payload + header->payloadLength);
  assembly.received[header->fragment] = true;
  ++assembly.receivedCount;
  if (assembly.receivedCount != assembly.fragmentCount) return;

  DecodedInput frame;
  frame.timestampUs = assembly.timestampUs;
  frame.keyframe = (assembly.flags & protocol::kKeyframe) != 0;
  if (needs_codec_config_ &&
      (assembly.flags & (protocol::kKeyframe | protocol::kCodecConfig)) !=
          (protocol::kKeyframe | protocol::kCodecConfig)) {
    ++frames_dropped_;
    assemblies_.erase(iterator);
    RequestKeyframe();
    return;
  }
  std::size_t total = 0;
  for (const auto& fragment : assembly.fragments) total += fragment.size();
  if (total == 0 || total > kMaxFrameBytes) {
    ++frames_dropped_;
  } else {
    frame.bytes.reserve(total);
    for (const auto& fragment : assembly.fragments) {
      frame.bytes.insert(frame.bytes.end(), fragment.begin(), fragment.end());
    }
    needs_codec_config_ = false;
    SubmitFrame(std::move(frame));
  }
  assemblies_.erase(iterator);
}

void ReceiverSession::SweepAssemblies() {
  const auto deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
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
  SendControl(R"({"type":"keyframe","reason":"loss_flush_or_session_start","requireCodecConfig":true})");
}

void ReceiverSession::CloseSockets() {
  CloseSocket(&control_socket_);
  CloseSocket(&video_socket_);
}

bool ReceiverSession::StartDecoder() {
  std::scoped_lock lifecycleLock(decoder_lifecycle_mutex_);
  DestroyDecoderLocked();
  return CreateDecoderLocked();
}

bool ReceiverSession::CreateDecoderLocked() {
  if (native_window_ == nullptr) return false;
  decoder_state_ = DecoderLifecycleState::kStarting;
  OH_AVCodec* decoder = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
  if (decoder == nullptr) {
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  OH_AVCodecCallback callbacks{OnCodecError, OnCodecStreamChanged,
                               OnCodecNeedInput, OnCodecOutput};
  if (OH_VideoDecoder_RegisterCallback(decoder, callbacks, this) != AV_ERR_OK ||
      OH_VideoDecoder_SetSurface(decoder, static_cast<OHNativeWindow*>(native_window_)) != AV_ERR_OK) {
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  OH_AVFormat* format = OH_AVFormat_CreateVideoFormat(OH_AVCODEC_MIMETYPE_VIDEO_AVC, 1920, 1200);
  if (format == nullptr) {
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  const OH_AVErrCode configure = OH_VideoDecoder_Configure(decoder, format);
  OH_AVFormat_Destroy(format);
  if (configure != AV_ERR_OK || OH_VideoDecoder_Prepare(decoder) != AV_ERR_OK) {
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  decoder_.store(decoder);
  decoder_state_ = DecoderLifecycleState::kRunning;
  if (OH_VideoDecoder_Start(decoder) != AV_ERR_OK) {
    decoder_state_ = DecoderLifecycleState::kStopping;
    ClearDecoderQueues();
    decoder_.store(nullptr);
    OH_VideoDecoder_Destroy(decoder);
    decoder_state_ = DecoderLifecycleState::kStopped;
    return false;
  }
  needs_codec_config_ = true;
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
  needs_codec_config_ = true;
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
    needs_codec_config_ = true;
  }
  if (recovered) RequestKeyframe();
  return recovered;
}

void ReceiverSession::SubmitFrame(DecodedInput frame) {
  std::scoped_lock queueLock(decoder_queue_mutex_);
  OH_AVCodec* decoder = decoder_.load();
  if (!DecoderCallbacksAllowed(decoder_state_.load()) || decoder == nullptr) {
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

void ReceiverSession::PumpDecoderLocked(OH_AVCodec* decoder) {
  while (DecoderCallbacksAllowed(decoder_state_.load()) && decoder_.load() == decoder &&
         !input_slots_.empty() && !decode_queue_.empty()) {
    InputSlot slot = input_slots_.front();
    input_slots_.pop_front();
    DecodedInput frame = std::move(decode_queue_.front());
    decode_queue_.pop_front();
    const int32_t capacity = OH_AVBuffer_GetCapacity(slot.buffer);
    auto* target = OH_AVBuffer_GetAddr(slot.buffer);
    if (capacity < 0 || target == nullptr || frame.bytes.size() > static_cast<std::size_t>(capacity)) {
      ++frames_dropped_;
      continue;
    }
    std::memcpy(target, frame.bytes.data(), frame.bytes.size());
    OH_AVCodecBufferAttr attributes{};
    attributes.pts = static_cast<int64_t>(frame.timestampUs);
    attributes.size = static_cast<int32_t>(frame.bytes.size());
    attributes.offset = 0;
    attributes.flags = frame.keyframe ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
    if (OH_AVBuffer_SetBufferAttr(slot.buffer, &attributes) != AV_ERR_OK ||
        OH_VideoDecoder_PushInputBuffer(decoder, slot.index) != AV_ERR_OK) {
      ++frames_dropped_;
    }
  }
}

void ReceiverSession::DecoderError(int32_t errorCode) {
  const bool connected = Status().connected;
  SetState("error", "AVCodec 解码错误: " + std::to_string(errorCode), connected);
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
      ++frames_decoded_;
      const auto estimatedHostRender = static_cast<std::int64_t>(ClockMicroseconds()) +
                                       host_clock_offset_us_.load();
      const auto endToEnd = std::max<std::int64_t>(0, estimatedHostRender - attributes.pts);
      std::ostringstream telemetry;
      telemetry << "{\"type\":\"telemetry\",\"captureUs\":" << attributes.pts
                << ",\"endToEndUs\":" << endToEnd
                << ",\"framesDecoded\":" << frames_decoded_.load()
                << ",\"framesDropped\":" << frames_dropped_.load() << "}";
      SendControl(telemetry.str());
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
  if (!StartDecoder()) SetState("error", "无法启动原生 AVCodec Surface 解码器", false);
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

void ReceiverSession::SetInputMode(std::string mode) {
  std::scoped_lock lock(input_mutex_);
  input_mode_ = mode == "scroll" ? "scroll" : "pointer";
  touch_active_ = false;
}

void ReceiverSession::OnTouch(OH_NativeXComponent* component, void* window) {
  OH_NativeXComponent_TouchEvent event{};
  std::uint64_t width = 0;
  std::uint64_t height = 0;
  if (OH_NativeXComponent_GetTouchEvent(component, window, &event) != 0 || event.numPoints != 1 ||
      OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) != 0 ||
      width == 0 || height == 0) {
    return;
  }
  const auto& point = event.touchPoints[0];
  const double x = std::clamp(static_cast<double>(point.x) / static_cast<double>(width), 0.0, 1.0);
  const double y = std::clamp(static_cast<double>(point.y) / static_cast<double>(height), 0.0, 1.0);
  std::string mode;
  {
    std::scoped_lock lock(input_mutex_);
    mode = input_mode_;
    if (mode == "scroll") {
      if (point.type == OH_NATIVEXCOMPONENT_DOWN) {
        previous_touch_y_ = point.y;
        touch_active_ = true;
        return;
      }
      if (point.type == OH_NATIVEXCOMPONENT_MOVE && touch_active_) {
        const double delta = static_cast<double>(point.y - previous_touch_y_) /
                             static_cast<double>(height);
        previous_touch_y_ = point.y;
        std::ostringstream json;
        json << "{\"type\":\"pointer\",\"action\":\"scroll\",\"pointerId\":0,\"x\":"
             << x << ",\"y\":" << y << ",\"deltaY\":" << delta
             << ",\"timestampUs\":" << ClockMicroseconds() << "}";
        SendControl(json.str());
      } else if (point.type == OH_NATIVEXCOMPONENT_UP ||
                 point.type == OH_NATIVEXCOMPONENT_CANCEL) {
        touch_active_ = false;
      }
      return;
    }
  }

  const char* action = nullptr;
  switch (point.type) {
    case OH_NATIVEXCOMPONENT_DOWN: action = "down"; break;
    case OH_NATIVEXCOMPONENT_MOVE: action = "move"; break;
    case OH_NATIVEXCOMPONENT_UP:
    case OH_NATIVEXCOMPONENT_CANCEL: action = "up"; break;
    default: return;
  }
  std::ostringstream json;
  json << "{\"type\":\"pointer\",\"action\":\"" << action
       << "\",\"pointerId\":0,\"x\":" << x << ",\"y\":" << y
       << ",\"buttons\":" << (point.type == OH_NATIVEXCOMPONENT_UP ? 0 : 1)
       << ",\"timestampUs\":" << ClockMicroseconds() << "}";
  SendControl(json.str());
}

}  // namespace hss::receiver
