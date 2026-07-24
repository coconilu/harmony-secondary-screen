#include "relay_protocol.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

void WriteBigEndian16(std::uint8_t* data, const std::uint16_t value) {
  data[0] = static_cast<std::uint8_t>(value >> 8U);
  data[1] = static_cast<std::uint8_t>(value);
}

void WriteBigEndian32(std::uint8_t* data, const std::uint32_t value) {
  data[0] = static_cast<std::uint8_t>(value >> 24U);
  data[1] = static_cast<std::uint8_t>(value >> 16U);
  data[2] = static_cast<std::uint8_t>(value >> 8U);
  data[3] = static_cast<std::uint8_t>(value);
}

void WriteBigEndian64(std::uint8_t* data, const std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    data[7 - index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

bool Expect(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

std::vector<std::uint8_t> CreateVideoMessage() {
  std::vector<std::uint8_t> message(
      hwc::relay::kLocalVideoHeaderSize + 3);
  WriteBigEndian32(message.data(), hwc::relay::kLocalVideoMagic);
  message[4] = hwc::relay::kLocalVideoVersion;
  message[5] = hwc::relay::kVideoFlagKeyFrame;
  WriteBigEndian16(message.data() + 6, hwc::relay::kLocalVideoHeaderSize);
  WriteBigEndian32(message.data() + 8, 42);
  WriteBigEndian32(message.data() + 12, 3);
  WriteBigEndian64(message.data() + 16, 1'234'567);
  message[24] = 0;
  message[25] = 0;
  message[26] = 1;
  return message;
}

}  // namespace

int main() {
  bool passed = true;

  const std::string accept =
      hwc::relay::CreateWebSocketAccept("dGhlIHNhbXBsZSBub25jZQ==");
  passed &= Expect(
      accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
      "RFC 6455 WebSocket accept value");

  const std::string token(64, 'a');
  passed &= Expect(
      hwc::relay::IsExpectedAuthMessage(
          "{\"type\":\"auth\",\"token\":\"" + token + "\"}",
          token),
      "exact authentication message");
  passed &= Expect(
      !hwc::relay::IsExpectedAuthMessage(
          "{\"type\":\"auth\",\"token\":\"wrong\"}",
          token),
      "wrong authentication token");

  passed &= Expect(
      hwc::relay::NormalizeExtensionOrigin(
          "chrome-extension://abcdefghijklmnopabcdefghijklmnop/") ==
          "chrome-extension://abcdefghijklmnopabcdefghijklmnop",
      "extension origin normalization");
  passed &= Expect(
      hwc::relay::NormalizeExtensionOrigin("https://example.com").empty(),
      "non-extension origin rejection");
  passed &= Expect(
      hwc::relay::JsonString(
          R"({"type":"configure_receiver","receiverAddress":"192.168.3.112"})",
          "receiverAddress") == "192.168.3.112",
      "JSON string extraction");
  passed &= Expect(
      hwc::relay::JsonUnsigned(
          R"({"protocol":2,"sessionShort":305419896})",
          "sessionShort") == 305419896U,
      "JSON unsigned extraction");
  passed &= Expect(
      !hwc::relay::JsonString(
          R"({"pairingCode":"123456","pairingCode":"654321"})",
          "pairingCode"),
      "duplicate JSON security field rejection");

  std::vector<std::uint8_t> message = CreateVideoMessage();
  hwc::relay::LocalVideoMessage parsed{};
  std::string error;
  passed &= Expect(
      hwc::relay::ParseLocalVideoMessage(message, &parsed, &error),
      "valid local video message");
  passed &= Expect(parsed.key_frame, "key-frame flag");
  passed &= Expect(parsed.sequence == 42, "frame sequence");
  passed &= Expect(parsed.timestamp_us == 1'234'567, "timestamp");
  passed &= Expect(parsed.payload.size() == 3, "payload length");

  message[12] = 0x7F;
  passed &= Expect(
      !hwc::relay::ParseLocalVideoMessage(message, &parsed, &error),
      "oversized payload rejection");

  return passed ? 0 : 1;
}
