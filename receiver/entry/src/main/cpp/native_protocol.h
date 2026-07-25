#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hss::receiver::protocol {

constexpr std::uint32_t kVideoMagic = 0x48574333U;
constexpr std::uint8_t kVersion = 3;
constexpr std::size_t kHeaderSize = 32;
constexpr std::uint32_t kMaxControlPayload = 64U * 1024U;
constexpr std::size_t kMaxFrameBytes = 8U * 1024U * 1024U;

enum VideoFlags : std::uint16_t {
  kKeyframe = 1U << 0U,
};

struct VideoHeader {
  std::uint32_t sourceEpoch = 0;
  std::uint32_t sequence = 0;
  std::uint16_t flags = 0;
  std::uint32_t payloadLength = 0;
  std::uint64_t timestampUs = 0;
};

std::optional<VideoHeader> DecodeVideoHeader(const std::byte* data, std::size_t size);

constexpr bool IsAcceptedSourceEpoch(std::uint32_t candidate, std::uint32_t latest) {
  return candidate >= latest;
}

std::optional<std::string> JsonString(std::string_view json, std::string_view key);
std::optional<std::int64_t> JsonInteger(std::string_view json, std::string_view key);
std::string EscapeJson(std::string_view value);

}  // namespace hss::receiver::protocol
