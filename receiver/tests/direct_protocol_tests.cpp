#include "native_protocol.h"
#include "websocket_protocol.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void WriteU32(std::byte* target, std::uint32_t value) {
  target[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
  target[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
  target[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
  target[3] = static_cast<std::byte>(value & 0xffU);
}

void DirectFrameAndEpochTest() {
  using namespace hss::receiver::protocol;
  constexpr std::array<std::byte, 5> annexB{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x65}};
  std::vector<std::byte> message(kHeaderSize + annexB.size());
  WriteU32(message.data(), kVideoMagic);
  message[4] = static_cast<std::byte>(kVersion);
  message[5] = static_cast<std::byte>(kKeyframe);
  message[6] = std::byte{0};
  message[7] = static_cast<std::byte>(kHeaderSize);
  WriteU32(message.data() + 8, 12);
  WriteU32(message.data() + 12, 7);
  WriteU32(message.data() + 16, annexB.size());
  message[31] = std::byte{55};
  std::copy(annexB.begin(), annexB.end(), message.begin() + kHeaderSize);
  const auto header = DecodeVideoHeader(message.data(), message.size());
  assert(header);
  assert(header->sourceEpoch == 12);
  assert(header->sequence == 7);
  assert(header->payloadLength == annexB.size());
  assert(IsAcceptedSourceEpoch(12, 12));
  assert(IsAcceptedSourceEpoch(13, 12));
  assert(!IsAcceptedSourceEpoch(11, 12));
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
  assert(hss::receiver::websocket::BuildUpgradeResponse(request, &response));
  assert(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
  assert(!hss::receiver::websocket::BuildUpgradeResponse(
      request.substr(0, request.find("/direct")) + "/other HTTP/1.1\r\n\r\n", &response));
}

}  // namespace

int main() {
  DirectFrameAndEpochTest();
  WebSocketHandshakeTest();
  return 0;
}
