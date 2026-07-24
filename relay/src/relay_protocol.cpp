#include "relay_protocol.h"

#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace hwc::relay {
namespace {

constexpr std::string_view kWebSocketGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

[[nodiscard]] std::optional<std::size_t> JsonValueStart(
    const std::string_view json,
    const std::string_view key) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  const std::size_t position = json.find(quoted);
  if (position == std::string_view::npos ||
      json.find(quoted, position + quoted.size()) != std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t colon = json.find(':', position + quoted.size());
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t start = colon + 1;
  while (start < json.size() &&
         (json[start] == ' ' || json[start] == '\t' ||
          json[start] == '\r' || json[start] == '\n')) {
    ++start;
  }
  return start;
}

[[nodiscard]] std::uint16_t ReadBigEndian16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(data[0]) << 8U) |
      static_cast<std::uint16_t>(data[1]));
}

[[nodiscard]] std::uint32_t ReadBigEndian32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) |
         static_cast<std::uint32_t>(data[3]);
}

[[nodiscard]] std::uint64_t ReadBigEndian64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(data[index]);
  }
  return value;
}

[[nodiscard]] std::string Base64Encode(
    std::span<const std::uint8_t> bytes) {
  if (bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
    throw std::runtime_error("base64 input is too large");
  }

  DWORD output_size = 0;
  if (CryptBinaryToStringA(
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
          nullptr,
          &output_size) == FALSE) {
    throw std::runtime_error("CryptBinaryToStringA size query failed");
  }

  std::string output(output_size, '\0');
  if (CryptBinaryToStringA(
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
          output.data(),
          &output_size) == FALSE) {
    throw std::runtime_error("CryptBinaryToStringA failed");
  }
  output.resize(output_size);
  return output;
}

}  // namespace

bool ParseLocalVideoMessage(
    const std::span<const std::uint8_t> message,
    LocalVideoMessage* const result,
    std::string* const error) {
  const auto fail = [error](const std::string_view message_text) {
    if (error != nullptr) {
      *error = std::string(message_text);
    }
    return false;
  };

  if (result == nullptr) {
    return fail("result is null");
  }
  if (message.size() < kLocalVideoHeaderSize) {
    return fail("message is shorter than the local video header");
  }
  if (ReadBigEndian32(message.data()) != kLocalVideoMagic) {
    return fail("local video magic does not match");
  }
  if (message[4] != kLocalVideoVersion) {
    return fail("local video version does not match");
  }
  const std::uint8_t flags = message[5];
  if ((flags & static_cast<std::uint8_t>(~kVideoFlagKeyFrame)) != 0) {
    return fail("local video flags contain unsupported bits");
  }
  if (ReadBigEndian16(message.data() + 6) != kLocalVideoHeaderSize) {
    return fail("local video header size does not match");
  }

  const std::uint32_t payload_length = ReadBigEndian32(message.data() + 12);
  if (payload_length > kMaxVideoPayloadBytes) {
    return fail("local video payload exceeds 8 MiB");
  }
  if (message.size() !=
      static_cast<std::size_t>(kLocalVideoHeaderSize) + payload_length) {
    return fail("local video payload length does not match message size");
  }

  result->key_frame = (flags & kVideoFlagKeyFrame) != 0;
  result->sequence = ReadBigEndian32(message.data() + 8);
  result->timestamp_us = ReadBigEndian64(message.data() + 16);
  result->payload = message.subspan(kLocalVideoHeaderSize);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::string CreateWebSocketAccept(const std::string_view client_key) {
  const std::string source = std::string(client_key) +
                             std::string(kWebSocketGuid);
  std::array<std::uint8_t, 20> digest{};
  const NTSTATUS status = BCryptHash(
      BCRYPT_SHA1_ALG_HANDLE,
      nullptr,
      0,
      reinterpret_cast<PUCHAR>(
          const_cast<char*>(source.data())),
      static_cast<ULONG>(source.size()),
      digest.data(),
      static_cast<ULONG>(digest.size()));
  if (status < 0) {
    throw std::runtime_error("BCryptHash SHA-1 failed");
  }
  return Base64Encode(digest);
}

std::string GenerateTokenHex(const std::size_t byte_count) {
  if (byte_count < 16 || byte_count > 64) {
    throw std::invalid_argument("token byte count must be between 16 and 64");
  }
  std::vector<std::uint8_t> bytes(byte_count);
  const NTSTATUS status = BCryptGenRandom(
      nullptr,
      bytes.data(),
      static_cast<ULONG>(bytes.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    throw std::runtime_error("BCryptGenRandom failed");
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const std::uint8_t value : bytes) {
    output << std::setw(2) << static_cast<unsigned int>(value);
  }
  return output.str();
}

bool IsExpectedAuthMessage(
    const std::string_view message,
    const std::string_view token) {
  const std::string expected =
      "{\"type\":\"auth\",\"token\":\"" + std::string(token) + "\"}";
  return message == expected;
}

std::string NormalizeExtensionOrigin(std::string_view origin) {
  while (!origin.empty() && origin.back() == '/') {
    origin.remove_suffix(1);
  }
  constexpr std::string_view prefix = "chrome-extension://";
  if (!origin.starts_with(prefix)) {
    return {};
  }
  const std::string_view extension_id = origin.substr(prefix.size());
  if (extension_id.size() != 32) {
    return {};
  }
  if (!std::all_of(
          extension_id.begin(),
          extension_id.end(),
          [](const char value) { return value >= 'a' && value <= 'p'; })) {
    return {};
  }
  return std::string(origin);
}

std::optional<std::string> JsonString(
    const std::string_view json,
    const std::string_view key) {
  const auto start = JsonValueStart(json, key);
  if (!start || *start >= json.size() || json[*start] != '"') {
    return std::nullopt;
  }
  std::string value;
  for (std::size_t index = *start + 1; index < json.size(); ++index) {
    const char character = json[index];
    if (character == '"') {
      return value;
    }
    if (character != '\\') {
      if (static_cast<unsigned char>(character) < 0x20U) {
        return std::nullopt;
      }
      value.push_back(character);
      continue;
    }
    if (++index >= json.size()) {
      return std::nullopt;
    }
    switch (json[index]) {
      case '"': value.push_back('"'); break;
      case '\\': value.push_back('\\'); break;
      case 'n': value.push_back('\n'); break;
      case 'r': value.push_back('\r'); break;
      case 't': value.push_back('\t'); break;
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> JsonUnsigned(
    const std::string_view json,
    const std::string_view key) {
  const auto start = JsonValueStart(json, key);
  if (!start) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto result = std::from_chars(
      json.data() + *start,
      json.data() + json.size(),
      value);
  if (result.ec != std::errc{}) {
    return std::nullopt;
  }
  return value;
}

std::string EscapeJson(const std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\r': output += "\\r"; break;
      case '\n': output += "\\n"; break;
      case '\t': output += "\\t"; break;
      default:
        if (static_cast<unsigned char>(character) >= 0x20U) {
          output += character;
        }
        break;
    }
  }
  return output;
}

}  // namespace hwc::relay
