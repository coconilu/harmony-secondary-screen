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

void WriteU32(std::byte* target, std::uint32_t value) {
  target[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
  target[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
  target[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
  target[3] = static_cast<std::byte>(value & 0xffU);
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
  header.flags = ReadU16(data + 6);
  header.session = ReadU32(data + 8);
  header.frame = ReadU32(data + 12);
  header.fragment = ReadU16(data + 16);
  header.fragments = ReadU16(data + 18);
  header.payloadLength = ReadU16(data + 20);
  header.timestampUs = ReadU64(data + 24);
  if (header.fragments == 0 || header.fragment >= header.fragments ||
      header.payloadLength > kMaxUdpPayload || size != kHeaderSize + header.payloadLength) {
    return std::nullopt;
  }
  return header;
}

std::vector<std::byte> EncodeControl(std::string_view json) {
  if (json.empty() || json.size() > kMaxControlPayload) return {};
  std::vector<std::byte> output(4 + json.size());
  WriteU32(output.data(), static_cast<std::uint32_t>(json.size()));
  std::memcpy(output.data() + 4, json.data(), json.size());
  return output;
}

bool ControlDecoder::Push(const std::byte* data, std::size_t size,
                          std::vector<std::string>* frames) {
  if (data == nullptr || frames == nullptr || buffer_.size() + size > kMaxControlPayload + 4U) {
    Reset();
    return false;
  }
  buffer_.insert(buffer_.end(), data, data + size);
  while (buffer_.size() >= 4) {
    const auto length = ReadU32(buffer_.data());
    if (length == 0 || length > kMaxControlPayload) {
      Reset();
      return false;
    }
    if (buffer_.size() < length + 4U) break;
    frames->emplace_back(reinterpret_cast<const char*>(buffer_.data() + 4), length);
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(length + 4U));
  }
  return true;
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
