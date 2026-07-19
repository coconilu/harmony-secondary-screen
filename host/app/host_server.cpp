#include "host_server.h"

#include "hss_protocol.h"
#include "local_security.h"
#include "network_gate.h"

#include <bcrypt.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace hss::host {
namespace {

constexpr std::uint16_t kControlPort = 47100;
constexpr std::uint16_t kVideoPort = 47101;
constexpr std::uint32_t kPipeMagic = 0x48535046U; // HSPF
constexpr std::uint32_t kMaxEncodedFrame = 16U * 1024U * 1024U;

#pragma pack(push, 1)
struct PipeFrameHeader {
  std::uint32_t magic;
  std::uint32_t payloadSize;
  std::uint32_t frame;
  std::uint32_t flags;
  std::uint64_t timestampUs;
};
#pragma pack(pop)

static_assert(sizeof(PipeFrameHeader) == 24);

bool ReadExactly(HANDLE pipe, HANDLE stopEvent, void* destination, DWORD bytes) {
  auto* output = static_cast<std::byte*>(destination);
  DWORD total = 0;
  while (total < bytes) {
    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
      return false;
    }
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
      return false;
    }
    if (available == 0) {
      WaitForSingleObject(stopEvent, 5);
      continue;
    }
    DWORD read = 0;
    const DWORD wanted = std::min(bytes - total, available);
    if (!ReadFile(pipe, output + total, wanted, &read, nullptr) || read == 0) {
      return false;
    }
    total += read;
  }
  return true;
}

std::string HexSession(const std::array<std::byte, 16>& value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const auto byte : value) {
    stream << std::setw(2) << static_cast<unsigned>(std::to_integer<std::uint8_t>(byte));
  }
  return stream.str();
}

bool SecureRandom(std::span<std::byte> output) {
  return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output.data()),
                         static_cast<ULONG>(output.size()),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::uint64_t ClockMicroseconds() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<std::uint64_t>((counter.QuadPart * 1'000'000LL) / frequency.QuadPart);
}

std::string SocketAddress(const sockaddr_in& address) {
  char ip[INET_ADDRSTRLEN]{};
  InetNtopA(AF_INET, const_cast<IN_ADDR*>(&address.sin_addr), ip,
            static_cast<DWORD>(std::size(ip)));
  return ip;
}

std::filesystem::path MetricsPath(std::string_view filename) {
  std::array<wchar_t, MAX_PATH> programData{};
  const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData.data(),
                                                static_cast<DWORD>(programData.size()));
  if (length > 0 && length < programData.size()) {
    const auto directory = std::filesystem::path(programData.data()) / L"HarmonySecondaryScreen";
    std::error_code createError;
    std::filesystem::create_directories(directory, createError);
    if (!createError) return directory / std::string(filename);
  }
  return std::filesystem::path(std::string(filename));
}

}  // namespace

HostServer::HostServer() {
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  LocalSecurityAttributes keyframeSecurity(kKeyframeEventSddl);
  keyframe_event_ = keyframeSecurity.valid()
                        ? CreateEventW(keyframeSecurity.get(), FALSE, FALSE,
                                       L"Global\\HarmonySecondaryScreen.RequestKeyframe")
                        : nullptr;
}

HostServer::~HostServer() {
  Stop();
  if (control_thread_.joinable()) {
    control_thread_.join();
  }
  if (pipe_thread_.joinable()) {
    pipe_thread_.join();
  }
  if (status_thread_.joinable()) {
    status_thread_.join();
  }
  if (udp_socket_ != INVALID_SOCKET) {
    closesocket(udp_socket_);
  }
  WSACleanup();
  if (keyframe_event_ != nullptr) {
    CloseHandle(keyframe_event_);
  }
  if (stop_event_ != nullptr) {
    CloseHandle(stop_event_);
  }
}

bool HostServer::Start(std::string* error) {
  if (error == nullptr || started_.exchange(true)) {
    return false;
  }
  if (stop_event_ == nullptr || keyframe_event_ == nullptr) {
    *error = "CreateEvent failed";
    return false;
  }
  if (WSAStartup(MAKEWORD(2, 2), &winsock_) != 0) {
    *error = "WSAStartup failed";
    return false;
  }
  udp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp_socket_ == INVALID_SOCKET) {
    *error = "UDP socket failed: " + std::to_string(WSAGetLastError());
    return false;
  }

  auto addresses = NetworkGate::TrustedWifiIpv4Addresses(error);
  if (addresses.empty()) {
    if (error->empty()) {
      *error = "没有可信物理 Wi-Fi IPv4 地址；未经确认的公用网络下 Host 拒绝监听";
    }
    return false;
  }
  RotatePairingCode();
  control_thread_ = std::thread(&HostServer::ControlLoop, this);
  pipe_thread_ = std::thread(&HostServer::PipeLoop, this);
  status_thread_ = std::thread(&HostServer::StatusLoop, this);
  return true;
}

void HostServer::Wait() {
  WaitForSingleObject(stop_event_, INFINITE);
}

void HostServer::Stop() {
  if (stop_event_ != nullptr) {
    SetEvent(stop_event_);
  }
  const SOCKET client = active_client_.load();
  if (client != INVALID_SOCKET) {
    shutdown(client, SD_BOTH);
  }
}

std::string HostServer::pairing_code() const {
  std::scoped_lock lock(state_mutex_);
  return pairing_code_;
}

void HostServer::RotatePairingCode() {
  std::array<std::byte, 4> random{};
  if (!SecureRandom(random)) {
    std::scoped_lock lock(state_mutex_);
    pairing_code_ = "------";
    return;
  }
  std::uint32_t value = 0;
  std::memcpy(&value, random.data(), sizeof(value));
  value = 100000U + (value % 900000U);
  std::scoped_lock lock(state_mutex_);
  pairing_code_ = std::to_string(value);
  paired_ = false;
}

void HostServer::ClearSession() {
  data_plane_gate_.Revoke();
  {
    std::scoped_lock lock(state_mutex_);
    paired_ = false;
    resume_pending_ = false;
    session_short_ = 0;
    video_peer_ = {};
  }
  RotatePairingCode();
  std::cout << "新的一次性配对码: " << pairing_code() << '\n';
}

void HostServer::MarkDisconnected() {
  std::scoped_lock lock(state_mutex_);
  if (paired_) {
    resume_pending_ = true;
    resume_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  }
}

void HostServer::ExpireResumeWindow() {
  bool expired = false;
  {
    std::scoped_lock lock(state_mutex_);
    expired = resume_pending_ && std::chrono::steady_clock::now() >= resume_deadline_;
  }
  if (expired) {
    ClearSession();
  }
}

std::string HostServer::SessionMessage() const {
  std::scoped_lock lock(state_mutex_);
  std::ostringstream response;
  response << R"({"type":"session","protocol":1,"sessionId":")"
           << HexSession(session_id_) << R"(","sessionShort":)" << session_short_
           << R"(,"codec":"video/avc","width":1920,"height":1200,"fps":60,"videoPort":47101})";
  return response.str();
}

std::string HostServer::StatusMessage() const {
  std::scoped_lock lock(state_mutex_);
  return R"({"type":"host_status","pairingCode":")" + pairing_code_ +
         R"(","paired":)" + (paired_ ? "true" : "false") + "}";
}

void HostServer::RecordHostFrame(std::uint32_t frame, std::uint64_t captureUs,
                                 std::size_t bytes, std::size_t fragments, bool keyframe) {
  std::scoped_lock lock(metrics_mutex_);
  std::ofstream output(MetricsPath("hss-host-frames.csv"), std::ios::app);
  if (output.tellp() == 0) {
    output << "frame,capture_us,send_complete_us,encoded_bytes,udp_fragments,keyframe\n";
  }
  output << frame << ',' << captureUs << ',' << ClockMicroseconds() << ',' << bytes << ','
         << fragments << ',' << (keyframe ? 1 : 0) << '\n';
}

void HostServer::RecordReceiverTelemetry(std::string_view json) {
  const auto captureUs = protocol::JsonInteger(json, "captureUs");
  const auto endToEndUs = protocol::JsonInteger(json, "endToEndUs");
  const auto decoded = protocol::JsonInteger(json, "framesDecoded");
  const auto dropped = protocol::JsonInteger(json, "framesDropped");
  if (!captureUs || !endToEndUs || !decoded || !dropped) return;
  std::scoped_lock lock(metrics_mutex_);
  std::ofstream output(MetricsPath("hss-receiver-telemetry.csv"), std::ios::app);
  if (output.tellp() == 0) {
    output << "received_host_us,capture_us,end_to_end_us,frames_decoded,frames_dropped\n";
  }
  output << ClockMicroseconds() << ',' << *captureUs << ',' << *endToEndUs << ','
         << *decoded << ',' << *dropped << '\n';
}

void HostServer::ControlLoop() {
  while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    ExpireResumeWindow();
    std::string gateError;
    const auto addresses = NetworkGate::TrustedWifiIpv4Addresses(&gateError);
    if (addresses.empty()) {
      WaitForSingleObject(stop_event_, 500);
      continue;
    }
    for (const auto& address : addresses) {
      if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
        return;
      }
      if (!NetworkGate::IsTrustedWifiIpv4(address, &gateError)) continue;
      const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (listener == INVALID_SOCKET) {
        continue;
      }
      BOOL exclusive = TRUE;
      setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
      sockaddr_in bindAddress{};
      bindAddress.sin_family = AF_INET;
      bindAddress.sin_port = htons(kControlPort);
      if (InetPtonA(AF_INET, address.c_str(), &bindAddress.sin_addr) != 1 ||
          bind(listener, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR ||
          listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        continue;
      }
      active_listener_ = listener;
      std::cout << "仅监听可信物理 Wi-Fi " << address << ':' << kControlPort << '\n';

      fd_set readSet;
      FD_ZERO(&readSet);
      FD_SET(listener, &readSet);
      timeval timeout{0, 500000};
      const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
      // Revalidate immediately before accepting. A Private -> Public profile
      // transition therefore closes this listener instead of accepting a peer.
      if (ready > 0 && NetworkGate::IsTrustedWifiIpv4(address, &gateError)) {
        sockaddr_in peer{};
        int peerLength = sizeof(peer);
        const SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (client != INVALID_SOCKET) {
          active_client_ = client;
          HandleClient(client, peer, address);
          active_client_ = INVALID_SOCKET;
          closesocket(client);
          MarkDisconnected();
          std::cout << "控制连接中断，保留会话 5 秒用于自动恢复\n";
        }
      }
      active_listener_ = INVALID_SOCKET;
      closesocket(listener);
    }
  }
}

bool HostServer::SendControl(SOCKET client, std::string_view json) {
  const auto frame = protocol::EncodeControlFrame(json);
  if (frame.empty()) {
    return false;
  }
  std::size_t sent = 0;
  while (sent < frame.size()) {
    const int result = send(client, reinterpret_cast<const char*>(frame.data() + sent),
                            static_cast<int>(frame.size() - sent), 0);
    if (result <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(result);
  }
  return true;
}

void HostServer::HandleClient(SOCKET client, sockaddr_in peer, std::string localAddress) {
  DWORD timeoutMs = 1000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
             sizeof(timeoutMs));
  std::cout << "接收来自 " << SocketAddress(peer) << " 的控制连接，等待一次性配对码\n";

  protocol::ControlFrameDecoder decoder;
  std::array<std::byte, 8192> receiveBuffer{};
  auto lastMessage = std::chrono::steady_clock::now();
  bool authenticated = false;

  while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    std::string gateError;
    if (!NetworkGate::IsTrustedWifiIpv4(localAddress, &gateError)) {
      ClearSession();
      SendControl(client, R"({"type":"error","code":"network_not_private"})");
      break;
    }
    const int received = recv(client, reinterpret_cast<char*>(receiveBuffer.data()),
                              static_cast<int>(receiveBuffer.size()), 0);
    if (received == 0) {
      break;
    }
    if (received < 0) {
      const int socketError = WSAGetLastError();
      if (socketError == WSAETIMEDOUT || socketError == WSAEWOULDBLOCK) {
        if (std::chrono::steady_clock::now() - lastMessage > std::chrono::seconds(3)) {
          break;
        }
        continue;
      }
      break;
    }
    lastMessage = std::chrono::steady_clock::now();
    std::vector<std::string> frames;
    std::string parseError;
    if (!decoder.Push(std::span(receiveBuffer).first(static_cast<std::size_t>(received)),
                      &frames, &parseError)) {
      SendControl(client, R"({"type":"error","code":"bad_frame"})");
      break;
    }

    for (const auto& json : frames) {
      const auto type = protocol::JsonString(json, "type");
      if (!authenticated) {
        const auto code = protocol::JsonString(json, "pairingCode");
        const auto requestedSession = protocol::JsonString(json, "sessionId");
        const auto version = protocol::JsonInteger(json, "protocol");
        const auto port = protocol::JsonInteger(json, "videoPort");
        bool canResume = false;
        bool codeMatches = false;
        {
          std::scoped_lock lock(state_mutex_);
          canResume = type == "resume" && requestedSession &&
                      *requestedSession == HexSession(session_id_) && paired_ && resume_pending_ &&
                      std::chrono::steady_clock::now() < resume_deadline_ &&
                      peer.sin_addr.s_addr == video_peer_.sin_addr.s_addr;
          codeMatches = type == "pair" && code && *code == pairing_code_ && !paired_;
        }
        if ((!canResume && !codeMatches) || version != protocol::kProtocolVersion ||
            !port || *port != kVideoPort) {
          SendControl(client, R"({"type":"error","code":"pairing_rejected"})");
          return;
        }

        if (canResume) {
          {
            std::scoped_lock lock(state_mutex_);
            resume_pending_ = false;
            video_peer_ = peer;
            video_peer_.sin_port = htons(kVideoPort);
          }
          if (!SendControl(client, SessionMessage())) return;
          authenticated = true;
          data_plane_gate_.Open();
          SetEvent(keyframe_event_);
          std::cout << "会话已在 5 秒窗口内恢复\n";
          continue;
        }

        if (!SecureRandom(session_id_)) {
          SendControl(client, R"({"type":"error","code":"random_failed"})");
          return;
        }
        std::uint32_t shortId = 0;
        std::memcpy(&shortId, session_id_.data(), sizeof(shortId));
        if (shortId == 0) {
          shortId = 1;
        }
        {
          std::scoped_lock lock(state_mutex_);
          paired_ = true;
          resume_pending_ = false;
          session_short_ = shortId;
          video_peer_ = peer;
          video_peer_.sin_port = htons(kVideoPort);
        }
        if (!SendControl(client, SessionMessage())) {
          return;
        }
        authenticated = true;
        data_plane_gate_.Open();
        SetEvent(keyframe_event_);
        std::cout << "配对成功，会话锁定到 " << SocketAddress(peer) << '\n';
        continue;
      }

      if (type == "ping") {
        const auto clientSend = protocol::JsonInteger(json, "clientSendUs").value_or(0);
        const auto serverReceive = ClockMicroseconds();
        const auto serverSend = ClockMicroseconds();
        SendControl(client, "{\"type\":\"pong\",\"clientSendUs\":" +
                                std::to_string(clientSend) + ",\"serverReceiveUs\":" +
                                std::to_string(serverReceive) + ",\"serverSendUs\":" +
                                std::to_string(serverSend) + "}");
      } else if (type == "keyframe") {
        SetEvent(keyframe_event_);
      } else if (type == "telemetry") {
        RecordReceiverTelemetry(json);
      } else if (type == "pointer") {
        std::string inputError;
        if (!pointer_relay_.Send(json, &inputError)) {
          SendControl(client, "{\"type\":\"error\",\"code\":\"pointer_rejected\",\"message\":\"" +
                                  protocol::EscapeJson(inputError) + "\"}");
        }
      }
    }
  }
}

void HostServer::PipeLoop() {
  while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    LocalSecurityAttributes frameSecurity(kFramesPipeSddl);
    if (!frameSecurity.valid()) {
      WaitForSingleObject(stop_event_, 500);
      continue;
    }
    HANDLE pipe = CreateNamedPipeW(
        L"\\\\.\\pipe\\HarmonySecondaryScreen.Frames", PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        64U * 1024U, 64U * 1024U, 1000, frameSecurity.get());
    if (pipe == INVALID_HANDLE_VALUE) {
      WaitForSingleObject(stop_event_, 500);
      continue;
    }
    BOOL connected = FALSE;
    while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
      if (ConnectNamedPipe(pipe, nullptr)) {
        connected = TRUE;
        break;
      }
      const DWORD connectError = GetLastError();
      if (connectError == ERROR_PIPE_CONNECTED) {
        connected = TRUE;
        break;
      }
      if (connectError != ERROR_PIPE_LISTENING && connectError != ERROR_NO_DATA) {
        break;
      }
      WaitForSingleObject(stop_event_, 50);
    }
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }
    while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
      PipeFrameHeader header{};
      if (!ReadExactly(pipe, stop_event_, &header, sizeof(header)) || header.magic != kPipeMagic ||
          header.payloadSize == 0 || header.payloadSize > kMaxEncodedFrame) {
        break;
      }
      std::vector<std::byte> payload(header.payloadSize);
      if (!ReadExactly(pipe, stop_event_, payload.data(), header.payloadSize)) {
        break;
      }
      SendVideoFrame(header.frame, header.timestampUs, header.flags, payload);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

void HostServer::StatusLoop() {
  while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    HANDLE pipe = CreateNamedPipeW(
        L"\\\\.\\pipe\\HarmonySecondaryScreen.Status", PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        4096, 4096, 1000, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      WaitForSingleObject(stop_event_, 500);
      continue;
    }
    bool connected = false;
    while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
      if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
        connected = true;
        break;
      }
      const DWORD error = GetLastError();
      if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA) break;
      WaitForSingleObject(stop_event_, 50);
    }
    if (connected) {
      const auto frame = protocol::EncodeControlFrame(StatusMessage());
      DWORD written = 0;
      WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);
      FlushFileBuffers(pipe);
      DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
  }
}

void HostServer::SendVideoFrame(std::uint32_t frame, std::uint64_t timestampUs,
                                std::uint32_t flags, const std::vector<std::byte>& payload) {
  const auto dataPlaneToken = data_plane_gate_.Capture();
  if (!dataPlaneToken) return;
  sockaddr_in peer{};
  std::uint32_t session = 0;
  {
    std::scoped_lock lock(state_mutex_);
    if (!paired_ || session_short_ == 0) {
      return;
    }
    peer = video_peer_;
    session = session_short_;
  }

  const std::size_t fragmentCount =
      (payload.size() + protocol::kMaxUdpPayload - 1U) / protocol::kMaxUdpPayload;
  if (fragmentCount == 0 || fragmentCount > std::numeric_limits<std::uint16_t>::max()) {
    return;
  }
  for (std::size_t fragment = 0; fragment < fragmentCount; ++fragment) {
    const std::size_t offset = fragment * protocol::kMaxUdpPayload;
    const std::size_t length = std::min(protocol::kMaxUdpPayload, payload.size() - offset);
    protocol::VideoFlags packetFlags = protocol::VideoFlags::kNone;
    if ((flags & 1U) != 0) {
      packetFlags = packetFlags | protocol::VideoFlags::kKeyframe;
    }
    if ((flags & 2U) != 0) {
      packetFlags = packetFlags | protocol::VideoFlags::kCodecConfig;
    }
    if (fragment + 1U == fragmentCount) {
      packetFlags = packetFlags | protocol::VideoFlags::kEndOfFrame;
    }
    protocol::VideoHeader videoHeader{session,
                                      frame,
                                      static_cast<std::uint16_t>(fragment),
                                      static_cast<std::uint16_t>(fragmentCount),
                                      packetFlags,
                                      static_cast<std::uint16_t>(length),
                                      timestampUs};
    const auto encodedHeader = protocol::EncodeVideoHeader(videoHeader);
    std::array<std::byte, protocol::kVideoHeaderSize + protocol::kMaxUdpPayload> datagram{};
    std::memcpy(datagram.data(), encodedHeader.data(), encodedHeader.size());
    std::memcpy(datagram.data() + encodedHeader.size(), payload.data() + offset, length);
    const bool sent = data_plane_gate_.RunIfAllowed(*dataPlaneToken, [&] {
      sendto(udp_socket_, reinterpret_cast<const char*>(datagram.data()),
             static_cast<int>(encodedHeader.size() + length), 0,
             reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    });
    if (!sent) return;
  }
  if (data_plane_gate_.CanSend(*dataPlaneToken)) {
    RecordHostFrame(frame, timestampUs, payload.size(), fragmentCount, (flags & 1U) != 0);
  }
}

}  // namespace hss::host
