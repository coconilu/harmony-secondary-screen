#include "native_protocol.h"

#include <charconv>
#include <cstring>

namespace hss::receiver::protocol {
namespace {

std::uint16_t ReadU16(const std::byte* source) {
  return static_cast<std::uint16_t>((std::to_integer<std::uint8_t>(source[0]) << 8U) |
                                    std::to_integer<std::uint8_t>(source[1]));
}

std::uint32_t ReadU32(const std::byte* source) {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[0])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[1])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[2])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[3]));
}

std::uint64_t ReadU64(const std::byte* source) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8U) | std::to_integer<std::uint8_t>(source[index]);
  }
  return value;
}

std::optional<std::size_t> ValueStart(std::string_view json, std::string_view key) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  const auto keyPosition = json.find(quoted);
  if (keyPosition == std::string_view::npos) return std::nullopt;
  const auto colon = json.find(':', keyPosition + quoted.size());
  if (colon == std::string_view::npos) return std::nullopt;
  std::size_t start = colon + 1;
  while (start < json.size() && (json[start] == ' ' || json[start] == '\t' ||
                                 json[start] == '\r' || json[start] == '\n')) {
    ++start;
  }
  return start;
}

}  // namespace

std::optional<VideoHeader> DecodeVideoHeader(const std::byte* data, std::size_t size) {
  if (data == nullptr || size < kHeaderSize || ReadU32(data) != kVideoMagic ||
      std::to_integer<std::uint8_t>(data[4]) != kVersion ||
      std::to_integer<std::uint8_t>(data[5]) != kHeaderSize) {
    return std::nullopt;
  }
  VideoHeader header;
  header.flags = std::to_integer<std::uint8_t>(data[5]);
  header.sourceEpoch = ReadU32(data + 8);
  header.sequence = ReadU32(data + 12);
  header.payloadLength = ReadU32(data + 16);
  header.timestampUs = ReadU64(data + 24);
  if (ReadU16(data + 6) != kHeaderSize || ReadU32(data + 20) != 0 ||
      header.payloadLength == 0 || header.payloadLength > kMaxFrameBytes ||
      (header.flags & ~kKeyframe) != 0 ||
      size != kHeaderSize + header.payloadLength) {
    return std::nullopt;
  }
  return header;
}

std::optional<std::string> JsonString(std::string_view json, std::string_view key) {
  const auto start = ValueStart(json, key);
  if (!start || *start >= json.size() || json[*start] != '"') return std::nullopt;
  std::string value;
  for (std::size_t index = *start + 1; index < json.size(); ++index) {
    if (json[index] == '"') return value;
    if (json[index] == '\\') {
      if (++index >= json.size()) return std::nullopt;
      switch (json[index]) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return std::nullopt;
      }
    } else {
      value.push_back(json[index]);
    }
  }
  return std::nullopt;
}

std::optional<std::int64_t> JsonInteger(std::string_view json, std::string_view key) {
  const auto start = ValueStart(json, key);
  if (!start) return std::nullopt;
  std::int64_t value = 0;
  const auto result = std::from_chars(json.data() + *start, json.data() + json.size(), value);
  return result.ec == std::errc{} ? std::optional<std::int64_t>(value) : std::nullopt;
}

std::string EscapeJson(std::string_view value) {
  std::string output;
  for (const char character : value) {
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output.push_back(character); break;
    }
  }
  return output;
}

}  // namespace hss::receiver::protocol
