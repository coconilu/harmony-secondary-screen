#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <optional>

namespace hwc::relay {

constexpr std::uint32_t kLocalVideoMagic = 0x48574C31;  // HWL1
constexpr std::uint8_t kLocalVideoVersion = 1;
constexpr std::uint16_t kLocalVideoHeaderSize = 24;
constexpr std::size_t kMaxVideoPayloadBytes = 8U * 1024U * 1024U;
constexpr std::uint8_t kVideoFlagKeyFrame = 1U << 0U;

struct LocalVideoMessage {
  bool key_frame = false;
  std::uint32_t sequence = 0;
  std::uint64_t timestamp_us = 0;
  std::span<const std::uint8_t> payload;
};

[[nodiscard]] bool ParseLocalVideoMessage(
    std::span<const std::uint8_t> message,
    LocalVideoMessage* result,
    std::string* error);

[[nodiscard]] std::string CreateWebSocketAccept(std::string_view client_key);
[[nodiscard]] std::string GenerateTokenHex(std::size_t byte_count = 32);
[[nodiscard]] bool IsExpectedAuthMessage(
    std::string_view message,
    std::string_view token);
[[nodiscard]] std::string NormalizeExtensionOrigin(std::string_view origin);
[[nodiscard]] std::optional<std::string> JsonString(
    std::string_view json,
    std::string_view key);
[[nodiscard]] std::optional<std::uint64_t> JsonUnsigned(
    std::string_view json,
    std::string_view key);
[[nodiscard]] std::string EscapeJson(std::string_view value);

}  // namespace hwc::relay
