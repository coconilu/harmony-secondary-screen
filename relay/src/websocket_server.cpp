#include "websocket_server.h"

#include "relay_protocol.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hwc::relay {
namespace {

constexpr std::size_t kMaxHandshakeBytes = 16U * 1024U;
constexpr std::size_t kMaxWebSocketMessageBytes =
    kLocalVideoHeaderSize + kMaxVideoPayloadBytes;
constexpr int kHandshakeTimeoutMs = 5000;

struct WebSocketFrame {
  bool final = false;
  std::uint8_t opcode = 0;
  std::vector<std::uint8_t> payload;
};

void CloseAtomicSocket(std::atomic<SOCKET>* const socket_value) {
  const SOCKET socket = socket_value->exchange(INVALID_SOCKET);
  if (socket != INVALID_SOCKET) {
    (void)shutdown(socket, SD_BOTH);
    (void)closesocket(socket);
  }
}

void SetSocketTimeout(
    const SOCKET socket,
    const int option,
    const int timeout_ms) {
  if (setsockopt(
          socket,
          SOL_SOCKET,
          option,
          reinterpret_cast<const char*>(&timeout_ms),
          sizeof(timeout_ms)) == SOCKET_ERROR) {
    throw std::runtime_error("failed to configure socket timeout");
  }
}

void SendAll(const SOCKET socket, const std::span<const std::uint8_t> data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk_size = static_cast<int>(std::min<std::size_t>(
        remaining,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int result = send(
        socket,
        reinterpret_cast<const char*>(data.data() + sent),
        chunk_size,
        0);
    if (result <= 0) {
      throw std::runtime_error("WebSocket send failed");
    }
    sent += static_cast<std::size_t>(result);
  }
}

void ReceiveExact(
    const SOCKET socket,
    const std::span<std::uint8_t> destination) {
  std::size_t received = 0;
  while (received < destination.size()) {
    const std::size_t remaining = destination.size() - received;
    const int chunk_size = static_cast<int>(std::min<std::size_t>(
        remaining,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int result = recv(
        socket,
        reinterpret_cast<char*>(destination.data() + received),
        chunk_size,
        0);
    if (result <= 0) {
      throw std::runtime_error("WebSocket connection closed");
    }
    received += static_cast<std::size_t>(result);
  }
}

[[nodiscard]] std::string ToLower(std::string value) {
  std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return value;
}

[[nodiscard]] std::string Trim(std::string value) {
  const auto not_space = [](const unsigned char character) {
    return std::isspace(character) == 0;
  };
  value.erase(
      value.begin(),
      std::find_if(value.begin(), value.end(), not_space));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), not_space).base(),
      value.end());
  return value;
}

[[nodiscard]] std::string ReceiveHandshake(const SOCKET client) {
  std::string request;
  request.reserve(2048);
  std::array<char, 1024> buffer{};
  while (request.find("\r\n\r\n") == std::string::npos) {
    const int received =
        recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0) {
      throw std::runtime_error("WebSocket handshake connection closed");
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
    if (request.size() > kMaxHandshakeBytes) {
      throw std::runtime_error("WebSocket handshake exceeds 16 KiB");
    }
  }
  return request;
}

[[nodiscard]] std::map<std::string, std::string> ParseHeaders(
    const std::string_view request,
    std::string* const request_line) {
  std::map<std::string, std::string> headers;
  std::size_t cursor = 0;
  bool first_line = true;
  while (cursor < request.size()) {
    const std::size_t end = request.find("\r\n", cursor);
    if (end == std::string_view::npos) {
      break;
    }
    const std::string line(request.substr(cursor, end - cursor));
    cursor = end + 2;
    if (line.empty()) {
      break;
    }
    if (first_line) {
      *request_line = line;
      first_line = false;
      continue;
    }
    const std::size_t separator = line.find(':');
    if (separator == std::string::npos) {
      throw std::runtime_error("malformed WebSocket header");
    }
    const std::string name = ToLower(Trim(line.substr(0, separator)));
    const std::string value = Trim(line.substr(separator + 1));
    if (name.empty() || headers.contains(name)) {
      throw std::runtime_error("duplicate or empty WebSocket header");
    }
    headers.emplace(name, value);
  }
  return headers;
}

void PerformHandshake(
    const SOCKET client,
    const std::string_view expected_origin) {
  const std::string request = ReceiveHandshake(client);
  std::string request_line;
  const auto headers = ParseHeaders(request, &request_line);
  if (request_line != "GET /capture HTTP/1.1") {
    throw std::runtime_error("unexpected WebSocket request target");
  }

  const auto require_header =
      [&headers](const std::string& name) -> const std::string& {
    const auto iterator = headers.find(name);
    if (iterator == headers.end()) {
      throw std::runtime_error("required WebSocket header is missing");
    }
    return iterator->second;
  };

  if (ToLower(require_header("upgrade")) != "websocket" ||
      ToLower(require_header("connection")).find("upgrade") ==
          std::string::npos ||
      require_header("sec-websocket-version") != "13" ||
      require_header("origin") != expected_origin) {
    throw std::runtime_error("WebSocket handshake policy rejected");
  }

  const std::string accept =
      CreateWebSocketAccept(require_header("sec-websocket-key"));
  const std::string response =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " +
      accept + "\r\n\r\n";
  SendAll(
      client,
      std::span(
          reinterpret_cast<const std::uint8_t*>(response.data()),
          response.size()));
}

[[nodiscard]] std::uint64_t ReadBigEndianLength(
    const std::span<const std::uint8_t> bytes) {
  std::uint64_t result = 0;
  for (const std::uint8_t value : bytes) {
    result = (result << 8U) | value;
  }
  return result;
}

[[nodiscard]] WebSocketFrame ReceiveFrame(const SOCKET client) {
  std::array<std::uint8_t, 2> header{};
  ReceiveExact(client, header);
  const bool final = (header[0] & 0x80U) != 0;
  const std::uint8_t reserved = header[0] & 0x70U;
  const std::uint8_t opcode = header[0] & 0x0FU;
  const bool masked = (header[1] & 0x80U) != 0;
  std::uint64_t payload_length = header[1] & 0x7FU;

  if (reserved != 0 || !masked) {
    throw std::runtime_error("unsupported WebSocket frame flags");
  }
  if (opcode != 0x0U && opcode != 0x1U && opcode != 0x2U &&
      opcode != 0x8U && opcode != 0x9U && opcode != 0xAU) {
    throw std::runtime_error("unsupported WebSocket opcode");
  }
  if (payload_length == 126) {
    std::array<std::uint8_t, 2> extended{};
    ReceiveExact(client, extended);
    payload_length = ReadBigEndianLength(extended);
  } else if (payload_length == 127) {
    std::array<std::uint8_t, 8> extended{};
    ReceiveExact(client, extended);
    if ((extended[0] & 0x80U) != 0) {
      throw std::runtime_error("invalid WebSocket 64-bit length");
    }
    payload_length = ReadBigEndianLength(extended);
  }
  if (payload_length > kMaxWebSocketMessageBytes) {
    throw std::runtime_error("WebSocket message exceeds local limit");
  }
  if (opcode >= 0x8U && payload_length > 125) {
    throw std::runtime_error("WebSocket control frame is too large");
  }
  if (opcode >= 0x8U && !final) {
    throw std::runtime_error("WebSocket control frame is fragmented");
  }

  std::array<std::uint8_t, 4> mask{};
  ReceiveExact(client, mask);
  WebSocketFrame frame;
  frame.final = final;
  frame.opcode = opcode;
  frame.payload.resize(static_cast<std::size_t>(payload_length));
  ReceiveExact(client, frame.payload);
  for (std::size_t index = 0; index < frame.payload.size(); ++index) {
    frame.payload[index] ^= mask[index % mask.size()];
  }
  return frame;
}

void SendFrame(
    const SOCKET client,
    const std::uint8_t opcode,
    const std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> frame;
  frame.reserve(10 + payload.size());
  frame.push_back(static_cast<std::uint8_t>(0x80U | opcode));
  if (payload.size() <= 125) {
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
  } else if (payload.size() <= 0xFFFFU) {
    frame.push_back(126);
    frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8U));
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
  } else {
    frame.push_back(127);
    const std::uint64_t length = payload.size();
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<std::uint8_t>(length >> shift));
    }
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  SendAll(client, frame);
}

void SendText(const SOCKET client, const std::string_view text) {
  SendFrame(
      client,
      0x1,
      std::span(
          reinterpret_cast<const std::uint8_t*>(text.data()),
          text.size()));
}

[[nodiscard]] WebSocketFrame ReceiveMessage(const SOCKET client) {
  std::uint8_t message_opcode = 0;
  std::vector<std::uint8_t> message_payload;

  while (true) {
    WebSocketFrame frame = ReceiveFrame(client);
    if (frame.opcode == 0x8U) {
      return frame;
    }
    if (frame.opcode == 0x9U) {
      SendFrame(client, 0xAU, frame.payload);
      continue;
    }
    if (frame.opcode == 0xAU) {
      continue;
    }

    if (frame.opcode == 0x0U) {
      if (message_opcode == 0) {
        throw std::runtime_error(
            "WebSocket continuation has no initial frame");
      }
    } else {
      if (message_opcode != 0) {
        throw std::runtime_error(
            "WebSocket data frame interrupted a fragmented message");
      }
      message_opcode = frame.opcode;
    }

    if (frame.payload.size() >
        kMaxWebSocketMessageBytes - message_payload.size()) {
      throw std::runtime_error(
          "fragmented WebSocket message exceeds local limit");
    }
    message_payload.insert(
        message_payload.end(),
        frame.payload.begin(),
        frame.payload.end());
    if (frame.final) {
      return WebSocketFrame{
          .final = true,
          .opcode = message_opcode,
          .payload = std::move(message_payload)};
    }
  }
}

}  // namespace

WebSocketServer::WebSocketServer(std::string expected_origin)
    : expected_origin_(std::move(expected_origin)),
      token_(GenerateTokenHex()) {}

WebSocketServer::~WebSocketServer() {
  Stop();
}

void WebSocketServer::Start() {
  if (worker_.joinable()) {
    throw std::runtime_error("WebSocket server already started");
  }

  WSADATA winsock_data{};
  if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
    throw std::runtime_error("WSAStartup failed");
  }
  winsock_started_ = true;

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    Stop();
    throw std::runtime_error("failed to create loopback listener");
  }
  listener_.store(listener);

  const BOOL exclusive = TRUE;
  if (setsockopt(
          listener,
          SOL_SOCKET,
          SO_EXCLUSIVEADDRUSE,
          reinterpret_cast<const char*>(&exclusive),
          sizeof(exclusive)) == SOCKET_ERROR) {
    Stop();
    throw std::runtime_error("failed to set exclusive loopback listener");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(
          listener,
          reinterpret_cast<const sockaddr*>(&address),
          sizeof(address)) == SOCKET_ERROR ||
      listen(listener, 1) == SOCKET_ERROR) {
    Stop();
    throw std::runtime_error("failed to bind loopback-only listener");
  }

  int address_size = sizeof(address);
  if (getsockname(
          listener,
          reinterpret_cast<sockaddr*>(&address),
          &address_size) == SOCKET_ERROR) {
    Stop();
    throw std::runtime_error("failed to read loopback listener port");
  }
  port_ = ntohs(address.sin_port);
  if (port_ == 0) {
    Stop();
    throw std::runtime_error("loopback listener returned port zero");
  }

  stopping_.store(false);
  worker_ = std::thread(&WebSocketServer::Run, this);
}

void WebSocketServer::Stop() {
  stopping_.store(true);
  CloseAtomicSocket(&client_);
  CloseAtomicSocket(&listener_);
  if (worker_.joinable()) {
    worker_.join();
  }
  lan_sender_.Stop();
  if (winsock_started_) {
    WSACleanup();
    winsock_started_ = false;
  }
  port_ = 0;
  std::fill(token_.begin(), token_.end(), '\0');
}

bool WebSocketServer::ConfigureReceiver(
    std::string receiver_address,
    std::string pairing_code,
    std::string* const error_code) {
  if (client_authenticated_.load()) {
    if (error_code != nullptr) {
      *error_code = "capture_already_started";
    }
    return false;
  }
  return lan_sender_.Configure(
      std::move(receiver_address),
      std::move(pairing_code),
      error_code);
}

std::uint16_t WebSocketServer::port() const noexcept {
  return port_;
}

const std::string& WebSocketServer::token() const noexcept {
  return token_;
}

void WebSocketServer::Run() {
  const SOCKET listener = listener_.load();
  if (listener == INVALID_SOCKET) {
    return;
  }
  const SOCKET accepted = accept(listener, nullptr, nullptr);
  if (accepted == INVALID_SOCKET || stopping_.load()) {
    if (accepted != INVALID_SOCKET) {
      (void)closesocket(accepted);
    }
    return;
  }
  client_.store(accepted);
  CloseAtomicSocket(&listener_);
  try {
    RunClient(accepted);
  } catch (const std::exception& error) {
    if (client_authenticated_.load() && !stopping_.load()) {
      try {
        const std::string message =
            "{\"type\":\"error\",\"code\":\"local_protocol_rejected\","
            "\"detail\":\"" +
            EscapeJson(error.what()) + "\"}";
        SendText(accepted, message);
        SendFrame(accepted, 0x8U, {});
      } catch (...) {
        // The client may already be gone.
      }
    }
  }
  client_authenticated_.store(false);
  CloseAtomicSocket(&client_);
  CloseAtomicSocket(&listener_);
}

void WebSocketServer::RunClient(const SOCKET client) {
  SetSocketTimeout(client, SO_RCVTIMEO, kHandshakeTimeoutMs);
  SetSocketTimeout(client, SO_SNDTIMEO, kHandshakeTimeoutMs);
  PerformHandshake(client, expected_origin_);

  const WebSocketFrame authentication = ReceiveMessage(client);
  if (authentication.opcode != 0x1U ||
      !IsExpectedAuthMessage(
          std::string_view(
              reinterpret_cast<const char*>(authentication.payload.data()),
              authentication.payload.size()),
          token_)) {
    throw std::runtime_error("local bridge authentication failed");
  }
  client_authenticated_.store(true);
  SendText(client, "{\"type\":\"ready\",\"protocol\":1}");
  if (lan_sender_.ConsumeKeyframeRequest()) {
    SendText(
        client,
        R"({"type":"keyframe","reason":"loss_flush_or_session_start","requireCodecConfig":true})");
  }

  SetSocketTimeout(client, SO_RCVTIMEO, 0);
  std::uint64_t received_frames = 0;
  std::uint64_t received_bytes = 0;
  std::uint64_t key_frames = 0;
  std::uint64_t invalid_messages = 0;
  auto last_telemetry = std::chrono::steady_clock::now();
  const auto send_telemetry = [&]() {
    const LanSenderStats lan = lan_sender_.stats();
    std::ostringstream telemetry;
    telemetry << "{\"type\":\"telemetry\",\"receivedFrames\":"
              << received_frames << ",\"receivedBytes\":"
              << received_bytes << ",\"keyFrames\":" << key_frames
              << ",\"invalidMessages\":" << invalid_messages
              << ",\"lanConnected\":"
              << (lan_sender_.connected() ? "true" : "false")
              << ",\"lanSentFrames\":" << lan.sent_frames
              << ",\"lanSentBytes\":" << lan.sent_bytes
              << ",\"lanSentDatagrams\":" << lan.sent_datagrams
              << ",\"lanSendErrors\":" << lan.send_errors
              << ",\"receiverDecodedFrames\":" << lan.receiver_decoded
              << ",\"receiverDroppedFrames\":" << lan.receiver_dropped
              << '}';
    SendText(client, telemetry.str());
  };

  while (!stopping_.load()) {
    WebSocketFrame frame = ReceiveMessage(client);
    if (frame.opcode == 0x8U) {
      SendFrame(client, 0x8U, frame.payload);
      return;
    }
    if (frame.opcode == 0x1U) {
      const std::string_view control(
          reinterpret_cast<const char*>(frame.payload.data()),
          frame.payload.size());
      if (control == "{\"type\":\"stats\"}") {
        send_telemetry();
        continue;
      }
      if (control == "{\"type\":\"close\"}") {
        send_telemetry();
        SendFrame(client, 0x8U, {});
        return;
      }
      ++invalid_messages;
      throw std::runtime_error("unexpected local control message");
    }
    if (frame.opcode != 0x2U) {
      ++invalid_messages;
      throw std::runtime_error("unexpected local WebSocket message type");
    }

    LocalVideoMessage video{};
    std::string parse_error;
    if (!ParseLocalVideoMessage(frame.payload, &video, &parse_error)) {
      ++invalid_messages;
      throw std::runtime_error("invalid local video message");
    }
    ++received_frames;
    received_bytes += video.payload.size();
    if (video.key_frame) {
      ++key_frames;
    }
    if (!lan_sender_.SendAccessUnit(video)) {
      throw std::runtime_error("LAN video send failed");
    }
    if (lan_sender_.ConsumeKeyframeRequest()) {
      SendText(
          client,
          R"({"type":"keyframe","reason":"loss_flush_or_session_start","requireCodecConfig":true})");
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_telemetry >= std::chrono::seconds(1)) {
      send_telemetry();
      last_telemetry = now;
    }
  }
}

}  // namespace hwc::relay
