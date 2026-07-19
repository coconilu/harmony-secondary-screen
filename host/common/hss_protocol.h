#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hss::protocol {

inline constexpr std::uint32_t kVideoMagic = 0x48535331U; // HSS1
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kVideoHeaderSize = 32;
inline constexpr std::uint32_t kMaxControlPayload = 64U * 1024U;
inline constexpr std::size_t kMaxUdpPayload = 1200;

enum class VideoFlags : std::uint16_t {
  kNone = 0,
  kKeyframe = 1U << 0U,
  kCodecConfig = 1U << 1U,
  kEndOfFrame = 1U << 2U,
};

constexpr VideoFlags operator|(VideoFlags lhs, VideoFlags rhs) noexcept {
  return static_cast<VideoFlags>(static_cast<std::uint16_t>(lhs) |
                                 static_cast<std::uint16_t>(rhs));
}

constexpr bool HasFlag(VideoFlags value, VideoFlags flag) noexcept {
  return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) != 0;
}

struct VideoHeader {
  std::uint32_t session = 0;
  std::uint32_t frame = 0;
  std::uint16_t fragment = 0;
  std::uint16_t fragments = 0;
  VideoFlags flags = VideoFlags::kNone;
  std::uint16_t payloadLength = 0;
  std::uint64_t timestampUs = 0;
};

std::array<std::byte, kVideoHeaderSize> EncodeVideoHeader(const VideoHeader& header);
std::optional<VideoHeader> DecodeVideoHeader(std::span<const std::byte> datagram);

std::vector<std::byte> EncodeControlFrame(std::string_view json);

class ControlFrameDecoder final {
 public:
  bool Push(std::span<const std::byte> bytes, std::vector<std::string>* frames,
            std::string* error);
  void Reset();

 private:
  std::vector<std::byte> buffer_;
};

std::optional<std::string> JsonString(std::string_view json, std::string_view key);
std::optional<std::int64_t> JsonInteger(std::string_view json, std::string_view key);
std::optional<double> JsonNumber(std::string_view json, std::string_view key);

std::string EscapeJson(std::string_view value);

}  // namespace hss::protocol
