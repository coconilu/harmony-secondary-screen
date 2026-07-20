#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hss::receiver::protocol {

constexpr std::uint32_t kVideoMagic = 0x48535331U;
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 32;
constexpr std::uint32_t kMaxControlPayload = 64U * 1024U;
constexpr std::size_t kMaxUdpPayload = 1200;

enum VideoFlags : std::uint16_t {
  kKeyframe = 1U << 0U,
  kCodecConfig = 1U << 1U,
  kEndOfFrame = 1U << 2U,
};

struct VideoHeader {
  std::uint32_t session = 0;
  std::uint32_t frame = 0;
  std::uint16_t fragment = 0;
  std::uint16_t fragments = 0;
  std::uint16_t flags = 0;
  std::uint16_t payloadLength = 0;
  std::uint64_t timestampUs = 0;
};

std::optional<VideoHeader> DecodeVideoHeader(const std::byte* data, std::size_t size);
std::vector<std::byte> EncodeControl(std::string_view json);

class ControlDecoder final {
 public:
  bool Push(const std::byte* data, std::size_t size, std::vector<std::string>* frames);
  void Reset() { buffer_.clear(); }

 private:
  std::vector<std::byte> buffer_;
};

std::optional<std::string> JsonString(std::string_view json, std::string_view key);
std::optional<std::int64_t> JsonInteger(std::string_view json, std::string_view key);
std::string EscapeJson(std::string_view value);

}  // namespace hss::receiver::protocol
