#include "native_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace hss::receiver::protocol {
namespace {

std::uint32_t ReadU32(const std::byte* source);

constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t RotateRight(std::uint32_t value, std::uint32_t count) {
  return (value >> count) | (value << (32U - count));
}

std::array<std::byte, 32> Sha256(const std::byte* data, std::size_t size) {
  std::vector<std::byte> message;
  message.reserve(size + 72U);
  if (data != nullptr && size > 0U) message.insert(message.end(), data, data + size);
  message.push_back(static_cast<std::byte>(0x80U));
  while (message.size() % 64U != 56U) message.push_back(std::byte{0});
  const std::uint64_t bitLength = static_cast<std::uint64_t>(size) * 8U;
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::byte>((bitLength >> shift) & 0xffU));
  }

  std::array<std::uint32_t, 8> hash{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      words[index] = ReadU32(message.data() + offset + index * 4U);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const std::uint32_t s0 =
          RotateRight(words[index - 15U], 7U) ^
          RotateRight(words[index - 15U], 18U) ^
          (words[index - 15U] >> 3U);
      const std::uint32_t s1 =
          RotateRight(words[index - 2U], 17U) ^
          RotateRight(words[index - 2U], 19U) ^
          (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    std::uint32_t a = hash[0];
    std::uint32_t b = hash[1];
    std::uint32_t c = hash[2];
    std::uint32_t d = hash[3];
    std::uint32_t e = hash[4];
    std::uint32_t f = hash[5];
    std::uint32_t g = hash[6];
    std::uint32_t h = hash[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t sigma1 =
          RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 =
          h + sigma1 + choose + kSha256Constants[index] + words[index];
      const std::uint32_t sigma0 =
          RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = sigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }
  std::array<std::byte, 32> digest{};
  for (std::size_t index = 0; index < hash.size(); ++index) {
    for (std::size_t byte = 0; byte < 4U; ++byte) {
      digest[index * 4U + byte] = static_cast<std::byte>(
          (hash[index] >> (24U - byte * 8U)) & 0xffU);
    }
  }
  return digest;
}

std::array<std::byte, 32> HmacSha256(const std::byte* key,
                                     std::size_t keySize,
                                     std::string_view message) {
  std::array<std::byte, 64> keyBlock{};
  if (keySize > keyBlock.size()) {
    const auto digest = Sha256(key, keySize);
    std::copy(digest.begin(), digest.end(), keyBlock.begin());
  } else if (key != nullptr && keySize > 0U) {
    std::copy(key, key + keySize, keyBlock.begin());
  }
  std::vector<std::byte> inner(keyBlock.size() + message.size());
  std::vector<std::byte> outer(keyBlock.size() + 32U);
  for (std::size_t index = 0; index < keyBlock.size(); ++index) {
    inner[index] = keyBlock[index] ^ static_cast<std::byte>(0x36U);
    outer[index] = keyBlock[index] ^ static_cast<std::byte>(0x5cU);
  }
  std::memcpy(inner.data() + keyBlock.size(), message.data(), message.size());
  const auto innerDigest = Sha256(inner.data(), inner.size());
  std::copy(innerDigest.begin(), innerDigest.end(),
            outer.begin() + static_cast<std::ptrdiff_t>(keyBlock.size()));
  return Sha256(outer.data(), outer.size());
}

std::string LowerHex(const std::byte* data, std::size_t size) {
  constexpr std::string_view alphabet = "0123456789abcdef";
  std::string output(size * 2U, '0');
  for (std::size_t index = 0; index < size; ++index) {
    const auto value = std::to_integer<std::uint8_t>(data[index]);
    output[index * 2U] = alphabet[value >> 4U];
    output[index * 2U + 1U] = alphabet[value & 0x0fU];
  }
  return output;
}

std::optional<std::vector<std::byte>> DecodeLowerHex(std::string_view value,
                                                      std::size_t byteCount) {
  if (value.size() != byteCount * 2U) return std::nullopt;
  std::vector<std::byte> output(byteCount);
  for (std::size_t index = 0; index < byteCount; ++index) {
    unsigned int decoded = 0;
    const auto result = std::from_chars(
        value.data() + index * 2U, value.data() + index * 2U + 2U, decoded, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + index * 2U + 2U ||
        decoded > 0xffU ||
        std::isupper(static_cast<unsigned char>(value[index * 2U])) != 0 ||
        std::isupper(static_cast<unsigned char>(value[index * 2U + 1U])) != 0) {
      return std::nullopt;
    }
    output[index] = static_cast<std::byte>(decoded);
  }
  return output;
}

bool SenderIdValid(std::string_view value) {
  return value.size() >= 16U && value.size() <= 64U &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f') ||
                  character == '-';
         });
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
      std::to_integer<std::uint8_t>(data[4]) != kVersion) {
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

std::string PairingShortCode(std::string_view token) {
  if (token.size() != 64U ||
      !std::all_of(token.begin(), token.end(), [](unsigned char character) {
        return std::isdigit(character) != 0 ||
               (character >= 'a' && character <= 'f');
      })) {
    return {};
  }
  std::uint64_t prefix = 0;
  const auto parsed = std::from_chars(token.data(), token.data() + 12, prefix, 16);
  if (parsed.ec != std::errc{}) return {};
  std::ostringstream output;
  output << std::setw(6) << std::setfill('0') << prefix % 1'000'000U;
  return output.str();
}

std::string PairProofMessage(std::string_view proofMode,
                             std::string_view sessionId,
                             std::string_view senderId,
                             std::string_view nonce,
                             std::string_view deviceId) {
  if (proofMode != "qr" ||
      !DecodeLowerHex(sessionId, 16U) ||
      !SenderIdValid(senderId) ||
      !DecodeLowerHex(nonce, 32U) ||
      !DecodeLowerHex(deviceId, 16U)) {
    return {};
  }
  return "HWC5-PAIR-PROOF\n" + std::string(proofMode) + "\n" +
         std::string(sessionId) + "\n" + std::string(senderId) + "\n" +
         std::string(nonce) + "\n" +
         std::string(deviceId);
}

std::string AuthProofMessage(std::string_view senderId,
                             std::string_view deviceId,
                             std::uint32_t sourceEpoch,
                             std::string_view nonce) {
  if (!SenderIdValid(senderId) || !DecodeLowerHex(deviceId, 16U) ||
      sourceEpoch == 0U || !DecodeLowerHex(nonce, 32U)) {
    return {};
  }
  return "HWC5-AUTH-PROOF\n" + std::string(senderId) + "\n" +
         std::string(deviceId) + "\n" + std::to_string(sourceEpoch) + "\n" +
         std::string(nonce) +
         "\nvideo/avc\nannexb\n1280\n720\n60";
}

std::string ComputePairProof(std::string_view secret,
                             std::string_view proofMode,
                             std::string_view sessionId,
                             std::string_view senderId,
                             std::string_view nonce,
                             std::string_view deviceId) {
  const std::string message =
      PairProofMessage(proofMode, sessionId, senderId, nonce, deviceId);
  if (message.empty()) return {};
  const auto decoded = DecodeLowerHex(secret, 32U);
  if (!decoded) return {};
  const auto& key = *decoded;
  const auto digest = HmacSha256(key.data(), key.size(), message);
  return LowerHex(digest.data(), digest.size());
}

std::string ComputeAuthProof(std::string_view credential,
                             std::string_view senderId,
                             std::string_view deviceId,
                             std::uint32_t sourceEpoch,
                             std::string_view nonce) {
  const auto key = DecodeLowerHex(credential, 32U);
  const std::string message =
      AuthProofMessage(senderId, deviceId, sourceEpoch, nonce);
  if (!key || message.empty()) return {};
  const auto digest = HmacSha256(key->data(), key->size(), message);
  return LowerHex(digest.data(), digest.size());
}

namespace {

bool ConstantTimeEqualSized(std::string_view first, std::string_view second,
                            std::size_t expectedSize) {
  std::size_t difference =
      (first.size() ^ second.size()) |
      (first.size() ^ expectedSize) |
      (second.size() ^ expectedSize);
  for (std::size_t index = 0; index < expectedSize; ++index) {
    const unsigned char left =
        index < first.size() ? static_cast<unsigned char>(first[index]) : 0U;
    const unsigned char right =
        index < second.size() ? static_cast<unsigned char>(second[index]) : 0U;
    difference |= left ^ right;
  }
  return difference == 0U;
}

}  // namespace

bool ConstantTimeEqual(std::string_view first, std::string_view second) {
  return ConstantTimeEqualSized(first, second, 64U);
}

PairingAuthorizationResult EvaluatePairingAuthorization(
    std::string_view sessionId, std::string_view token,
    std::string_view pendingSessionId, std::string_view pendingToken,
    std::string_view pendingShortCode, std::string_view consumedSessionId,
    std::int64_t expiresAtMs, std::int64_t nowMs) {
  if (!sessionId.empty() && sessionId == consumedSessionId) {
    return PairingAuthorizationResult::kReplayed;
  }
  if (nowMs >= expiresAtMs) {
    return PairingAuthorizationResult::kExpired;
  }
  const bool qrMatch =
      !pendingSessionId.empty() && sessionId == pendingSessionId &&
      ConstantTimeEqual(token, pendingToken);
  const bool shortMatch =
      !pendingShortCode.empty() &&
      ConstantTimeEqualSized(PairingShortCode(token), pendingShortCode, 6U);
  return qrMatch || shortMatch ? PairingAuthorizationResult::kAccepted
                              : PairingAuthorizationResult::kMismatch;
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
