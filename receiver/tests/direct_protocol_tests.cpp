#include "native_protocol.h"
#include "websocket_protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void Check(bool condition, std::string_view expression, int line) {
  if (!condition) {
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
    ++failures;
  }
}

#define CHECK(condition) Check(static_cast<bool>(condition), #condition, __LINE__)

std::vector<std::byte> ReadFixture(const char* path) {
  if (path == nullptr) return {};
  std::ifstream input(path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte> result(bytes.size());
  std::transform(bytes.begin(), bytes.end(), result.begin(),
                 [](char value) {
                   return static_cast<std::byte>(
                       static_cast<unsigned char>(value));
                 });
  return result;
}

void ExtensionWireVectorTest(const char* fixturePath) {
  using namespace hss::receiver::protocol;
  const auto message = ReadFixture(fixturePath);
  CHECK(message.size() == kHeaderSize + 15U);
  if (message.size() < kHeaderSize) return;

  const auto header = DecodeVideoHeader(message.data(), message.size());
  CHECK(header.has_value());
  if (!header) return;
  CHECK(header->flags == kKeyframe);
  CHECK(header->sourceEpoch == 9U);
  CHECK(header->sequence == 42U);
  CHECK(header->payloadLength == 15U);
  CHECK(header->timestampUs == 1'234'567U);
  const std::array<std::byte, 15> expected{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x67},
      std::byte{0x42}, std::byte{0}, std::byte{0x1f}, std::byte{0},
      std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x65},
      std::byte{0x88}, std::byte{0x84}};
  CHECK(std::equal(expected.begin(), expected.end(),
                   message.begin() + static_cast<std::ptrdiff_t>(kHeaderSize)));

  auto invalidHeaderSize = message;
  invalidHeaderSize[7] = std::byte{31};
  CHECK(!DecodeVideoHeader(invalidHeaderSize.data(), invalidHeaderSize.size()));
  auto legacyHwc3 = message;
  legacyHwc3[3] = std::byte{0x33};
  legacyHwc3[4] = std::byte{3};
  CHECK(!DecodeVideoHeader(legacyHwc3.data(), legacyHwc3.size()));
  auto invalidFlags = message;
  invalidFlags[5] = std::byte{0x80};
  CHECK(!DecodeVideoHeader(invalidFlags.data(), invalidFlags.size()));
  auto invalidPayloadLength = message;
  invalidPayloadLength[19] = std::byte{16};
  CHECK(!DecodeVideoHeader(invalidPayloadLength.data(),
                           invalidPayloadLength.size()));

  CHECK(IsAcceptedSourceEpoch(12, 12));
  CHECK(IsAcceptedSourceEpoch(13, 12));
  CHECK(!IsAcceptedSourceEpoch(11, 12));
}

std::vector<std::byte> MaskedFrame(
    hss::receiver::websocket::Opcode opcode,
    std::string_view payload,
    bool final = true) {
  const std::array<std::byte, 4> mask{
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
  std::vector<std::byte> frame{
      static_cast<std::byte>((final ? 0x80U : 0U) |
                             static_cast<std::uint8_t>(opcode)),
      static_cast<std::byte>(0x80U | payload.size())};
  frame.insert(frame.end(), mask.begin(), mask.end());
  for (std::size_t index = 0; index < payload.size(); ++index) {
    frame.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(payload[index]) ^
        std::to_integer<std::uint8_t>(mask[index % mask.size()])));
  }
  return frame;
}

void WebSocketDecoderTest() {
  using namespace hss::receiver::websocket;
  Decoder validDecoder;
  std::vector<Message> messages;
  const auto valid = MaskedFrame(Opcode::kText, "hello");
  CHECK(validDecoder.Push(valid.data(), valid.size(), &messages));
  CHECK(messages.size() == 1U);
  if (messages.size() == 1U) {
    CHECK(messages[0].opcode == Opcode::kText);
    CHECK(std::string(reinterpret_cast<const char*>(messages[0].payload.data()),
                      messages[0].payload.size()) == "hello");
  }

  Decoder unmaskedDecoder;
  const std::array<std::byte, 2> unmasked{
      std::byte{0x81}, std::byte{0}};
  messages.clear();
  CHECK(!unmaskedDecoder.Push(unmasked.data(), unmasked.size(), &messages));

  Decoder lengthDecoder;
  std::array<std::byte, 10> excessiveLength{};
  excessiveLength[0] = std::byte{0x82};
  excessiveLength[1] = std::byte{0xff};
  const std::uint64_t declaredLength = kMaxMessageBytes + 1U;
  for (std::size_t index = 0; index < 8U; ++index) {
    excessiveLength[2U + index] = static_cast<std::byte>(
        (declaredLength >> (56U - index * 8U)) & 0xffU);
  }
  CHECK(!lengthDecoder.Push(excessiveLength.data(), excessiveLength.size(),
                            &messages));

  Decoder controlDecoder;
  const auto fragmentedPing = MaskedFrame(Opcode::kPing, "", false);
  CHECK(!controlDecoder.Push(fragmentedPing.data(), fragmentedPing.size(),
                             &messages));
}

void WebSocketHandshakeTest() {
  std::string response;
  const std::string request =
      "GET /direct HTTP/1.1\r\n"
      "Host: 192.168.1.8:44000\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  CHECK(hss::receiver::websocket::BuildUpgradeResponse(request, &response));
  CHECK(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
        std::string::npos);
  CHECK(!hss::receiver::websocket::BuildUpgradeResponse(
      request.substr(0, request.find("/direct")) +
          "/other HTTP/1.1\r\n\r\n",
      &response));
}

void PairingAuthorizationNegativeTest() {
  using namespace hss::receiver::protocol;
  const std::string session(32, '1');
  const std::string token(64, '2');
  CHECK(EvaluatePairingAuthorization(
            session, token, session, token, "", "", 61'000, 60'999) ==
        PairingAuthorizationResult::kAccepted);
  CHECK(EvaluatePairingAuthorization(
            session, token, session, token, "", "", 61'000, 61'000) ==
        PairingAuthorizationResult::kExpired);
  CHECK(EvaluatePairingAuthorization(
            session, token, "", "", "", session, 120'000, 61'000) ==
        PairingAuthorizationResult::kReplayed);
  CHECK(EvaluatePairingAuthorization(
            session, std::string(64, '3'), session, token, "", "",
            120'000, 61'000) ==
        PairingAuthorizationResult::kMismatch);
}

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

class SocketRuntime {
 public:
  SocketRuntime() {
    WSADATA data{};
    ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~SocketRuntime() {
    if (ready_) WSACleanup();
  }
  bool ready() const { return ready_; }

 private:
  bool ready_ = false;
};

void CloseNativeSocket(NativeSocket socket) {
  if (socket != kInvalidSocket) closesocket(socket);
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

class SocketRuntime {
 public:
  bool ready() const { return true; }
};

void CloseNativeSocket(NativeSocket socket) {
  if (socket != kInvalidSocket) close(socket);
}
#endif

bool SendAll(NativeSocket socket, std::string_view payload) {
  std::size_t sent = 0;
  while (sent < payload.size()) {
    const int count =
        send(socket, payload.data() + sent,
             static_cast<int>(payload.size() - sent), 0);
    if (count <= 0) return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

NativeSocket ConnectLoopback(std::uint16_t port) {
  const NativeSocket socketValue = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socketValue == kInvalidSocket) return kInvalidSocket;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(socketValue, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) != 0) {
    CloseNativeSocket(socketValue);
    return kInvalidSocket;
  }
  return socketValue;
}

void SilentUpgradeDeadlineTest() {
  SocketRuntime runtime;
  CHECK(runtime.ready());
  if (!runtime.ready()) return;

  const NativeSocket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  CHECK(listener != kInvalidSocket);
  if (listener == kInvalidSocket) return;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
          0 ||
      listen(listener, 2) != 0) {
    CHECK(false);
    CloseNativeSocket(listener);
    return;
  }
  socklen_t addressSize = sizeof(address);
  CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                    &addressSize) == 0);
  const std::uint16_t port = ntohs(address.sin_port);

  struct ServerResult {
    bool silentRejected = false;
    bool nextAccepted = false;
    std::int64_t silentElapsedMs = 0;
  } result;
  std::thread server([&] {
    sockaddr_in peer{};
    socklen_t peerSize = sizeof(peer);
    const NativeSocket silent =
        accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerSize);
    if (silent == kInvalidSocket) return;
    std::string request;
    const auto started = std::chrono::steady_clock::now();
    result.silentRejected = !hss::receiver::websocket::ReadUpgradeRequest(
        static_cast<hss::receiver::websocket::SocketHandle>(silent),
        started + std::chrono::seconds(5), &request);
    result.silentElapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    CloseNativeSocket(silent);

    peerSize = sizeof(peer);
    const NativeSocket next =
        accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerSize);
    if (next == kInvalidSocket) return;
    request.clear();
    std::string response;
    result.nextAccepted =
        hss::receiver::websocket::ReadUpgradeRequest(
            static_cast<hss::receiver::websocket::SocketHandle>(next),
            std::chrono::steady_clock::now() + std::chrono::seconds(2),
            &request) &&
        hss::receiver::websocket::BuildUpgradeResponse(request, &response);
    CloseNativeSocket(next);
  });

  const NativeSocket silentClient = ConnectLoopback(port);
  CHECK(silentClient != kInvalidSocket);
  std::this_thread::sleep_for(std::chrono::milliseconds(5'250));
  const NativeSocket nextClient = ConnectLoopback(port);
  CHECK(nextClient != kInvalidSocket);
  const std::string request =
      "GET /direct HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  if (nextClient != kInvalidSocket) CHECK(SendAll(nextClient, request));
  CloseNativeSocket(nextClient);
  CloseNativeSocket(silentClient);
  server.join();
  CloseNativeSocket(listener);

  CHECK(result.silentRejected);
  CHECK(result.silentElapsedMs >= 4'900);
  CHECK(result.silentElapsedMs < 8'000);
  CHECK(result.nextAccepted);
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  ExtensionWireVectorTest(argc == 2 ? argv[1] : nullptr);
  WebSocketDecoderTest();
  WebSocketHandshakeTest();
  PairingAuthorizationNegativeTest();
  SilentUpgradeDeadlineTest();
  if (failures != 0) {
    std::cerr << failures << " receiver protocol check(s) failed\n";
    return 1;
  }
  std::cout << "All receiver protocol checks passed\n";
  return 0;
}
