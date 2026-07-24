#include "hss_protocol.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace hss::protocol {
namespace {

void WriteU16(std::byte* target, std::uint16_t value) {
  target[0] = static_cast<std::byte>((value >> 8U) & 0xffU);
  target[1] = static_cast<std::byte>(value & 0xffU);
}

void WriteU32(std::byte* target, std::uint32_t value) {
  target[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
  target[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
  target[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
  target[3] = static_cast<std::byte>(value & 0xffU);
}

void WriteU64(std::byte* target, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    target[index] = static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU);
  }
}

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

std::optional<std::size_t> FindValueStart(std::string_view json, std::string_view key) {
  const std::string quotedKey = "\"" + std::string(key) + "\"";
  const auto keyPosition = json.find(quotedKey);
  if (keyPosition == std::string_view::npos) {
    return std::nullopt;
  }
  const auto colonPosition = json.find(':', keyPosition + quotedKey.size());
  if (colonPosition == std::string_view::npos) {
    return std::nullopt;
  }
  auto valuePosition = colonPosition + 1;
  while (valuePosition < json.size() &&
         (json[valuePosition] == ' ' || json[valuePosition] == '\t' ||
          json[valuePosition] == '\r' || json[valuePosition] == '\n')) {
    ++valuePosition;
  }
  return valuePosition;
}

}  // namespace

std::array<std::byte, kVideoHeaderSize> EncodeVideoHeader(const VideoHeader& header) {
  std::array<std::byte, kVideoHeaderSize> bytes{};
  WriteU32(bytes.data(), kVideoMagic);
  bytes[4] = static_cast<std::byte>(kProtocolVersion);
  bytes[5] = static_cast<std::byte>(kVideoHeaderSize);
  WriteU16(bytes.data() + 6, static_cast<std::uint16_t>(header.flags));
  WriteU32(bytes.data() + 8, header.session);
  WriteU32(bytes.data() + 12, header.frame);
  WriteU16(bytes.data() + 16, header.fragment);
  WriteU16(bytes.data() + 18, header.fragments);
  WriteU16(bytes.data() + 20, header.payloadLength);
  WriteU16(bytes.data() + 22, 0);
  WriteU64(bytes.data() + 24, header.timestampUs);
  return bytes;
}

std::optional<VideoHeader> DecodeVideoHeader(std::span<const std::byte> datagram) {
  if (datagram.size() < kVideoHeaderSize || ReadU32(datagram.data()) != kVideoMagic ||
      std::to_integer<std::uint8_t>(datagram[4]) != kProtocolVersion ||
      std::to_integer<std::uint8_t>(datagram[5]) != kVideoHeaderSize) {
    return std::nullopt;
  }

  VideoHeader header;
  header.flags = static_cast<VideoFlags>(ReadU16(datagram.data() + 6));
  header.session = ReadU32(datagram.data() + 8);
  header.frame = ReadU32(datagram.data() + 12);
  header.fragment = ReadU16(datagram.data() + 16);
  header.fragments = ReadU16(datagram.data() + 18);
  header.payloadLength = ReadU16(datagram.data() + 20);
  header.timestampUs = ReadU64(datagram.data() + 24);

  if (header.fragments == 0 || header.fragment >= header.fragments ||
      header.payloadLength > kMaxUdpPayload ||
      datagram.size() != kVideoHeaderSize + header.payloadLength) {
    return std::nullopt;
  }
  return header;
}

std::vector<std::byte> EncodeControlFrame(std::string_view json) {
  if (json.size() > kMaxControlPayload) {
    return {};
  }
  std::vector<std::byte> frame(4 + json.size());
  WriteU32(frame.data(), static_cast<std::uint32_t>(json.size()));
  std::memcpy(frame.data() + 4, json.data(), json.size());
  return frame;
}

bool ControlFrameDecoder::Push(std::span<const std::byte> bytes,
                               std::vector<std::string>* frames,
                               std::string* error) {
  if (frames == nullptr || error == nullptr) {
    return false;
  }
  if (buffer_.size() + bytes.size() > kMaxControlPayload + 4U) {
    *error = "control buffer exceeds 64 KiB";
    Reset();
    return false;
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

  while (buffer_.size() >= 4) {
    const auto payloadLength = ReadU32(buffer_.data());
    if (payloadLength == 0 || payloadLength > kMaxControlPayload) {
      *error = "invalid control payload length";
      Reset();
      return false;
    }
    const auto totalLength = static_cast<std::size_t>(payloadLength) + 4U;
    if (buffer_.size() < totalLength) {
      break;
    }
    frames->emplace_back(reinterpret_cast<const char*>(buffer_.data() + 4), payloadLength);
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(totalLength));
  }
  return true;
}

void ControlFrameDecoder::Reset() {
  buffer_.clear();
}

std::optional<std::string> JsonString(std::string_view json, std::string_view key) {
  const auto start = FindValueStart(json, key);
  if (!start || *start >= json.size() || json[*start] != '"') {
    return std::nullopt;
  }
  std::string value;
  for (std::size_t index = *start + 1; index < json.size(); ++index) {
    const char current = json[index];
    if (current == '"') {
      return value;
    }
    if (current == '\\') {
      if (++index >= json.size()) {
        return std::nullopt;
      }
      switch (json[index]) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return std::nullopt;
      }
    } else {
      value.push_back(current);
    }
  }
  return std::nullopt;
}

std::optional<std::int64_t> JsonInteger(std::string_view json, std::string_view key) {
  const auto start = FindValueStart(json, key);
  if (!start) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const auto* begin = json.data() + *start;
  const auto* end = json.data() + json.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{}) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> JsonNumber(std::string_view json, std::string_view key) {
  const auto start = FindValueStart(json, key);
  if (!start) {
    return std::nullopt;
  }
  const auto end = json.find_first_of(",}\r\n\t ", *start);
  const auto token = json.substr(*start, end == std::string_view::npos ? json.size() - *start
                                                                      : end - *start);
  std::string copy(token);
  char* parseEnd = nullptr;
  const double value = std::strtod(copy.c_str(), &parseEnd);
  if (parseEnd != copy.c_str() + copy.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char current : value) {
    switch (current) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped.push_back(current); break;
    }
  }
  return escaped;
}

}  // namespace hss::protocol
