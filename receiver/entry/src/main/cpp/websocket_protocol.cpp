#include "websocket_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace hss::receiver::websocket {
namespace {

constexpr std::string_view kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::uint32_t RotateLeft(std::uint32_t value, unsigned int count) {
  return (value << count) | (value >> (32U - count));
}

std::array<std::byte, 20> Sha1(std::string_view input) {
  std::vector<std::byte> message(input.size());
  std::memcpy(message.data(), input.data(), input.size());
  message.push_back(static_cast<std::byte>(0x80));
  while (message.size() % 64U != 56U) message.push_back(std::byte{0});
  const std::uint64_t bitLength = static_cast<std::uint64_t>(input.size()) * 8U;
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::byte>((bitLength >> shift) & 0xffU));
  }

  std::uint32_t h0 = 0x67452301U;
  std::uint32_t h1 = 0xEFCDAB89U;
  std::uint32_t h2 = 0x98BADCFEU;
  std::uint32_t h3 = 0x10325476U;
  std::uint32_t h4 = 0xC3D2E1F0U;
  for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
    std::array<std::uint32_t, 80> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const auto* source = message.data() + offset + index * 4U;
      words[index] =
          (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[0])) << 24U) |
          (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[1])) << 16U) |
          (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[2])) << 8U) |
          static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[3]));
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      words[index] = RotateLeft(words[index - 3] ^ words[index - 8] ^
                                words[index - 14] ^ words[index - 16], 1);
    }
    std::uint32_t a = h0;
    std::uint32_t b = h1;
    std::uint32_t c = h2;
    std::uint32_t d = h3;
    std::uint32_t e = h4;
    for (std::size_t index = 0; index < words.size(); ++index) {
      std::uint32_t function = 0;
      std::uint32_t constant = 0;
      if (index < 20) {
        function = (b & c) | ((~b) & d);
        constant = 0x5A827999U;
      } else if (index < 40) {
        function = b ^ c ^ d;
        constant = 0x6ED9EBA1U;
      } else if (index < 60) {
        function = (b & c) | (b & d) | (c & d);
        constant = 0x8F1BBCDCU;
      } else {
        function = b ^ c ^ d;
        constant = 0xCA62C1D6U;
      }
      const std::uint32_t temporary =
          RotateLeft(a, 5) + function + e + constant + words[index];
      e = d;
      d = c;
      c = RotateLeft(b, 30);
      b = a;
      a = temporary;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }
  const std::array<std::uint32_t, 5> hashes{h0, h1, h2, h3, h4};
  std::array<std::byte, 20> digest{};
  for (std::size_t index = 0; index < hashes.size(); ++index) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      digest[index * 4U + byte] =
          static_cast<std::byte>((hashes[index] >> (24U - byte * 8U)) & 0xffU);
    }
  }
  return digest;
}

std::string Base64(const std::byte* data, std::size_t size) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  for (std::size_t offset = 0; offset < size; offset += 3U) {
    const std::uint32_t a = std::to_integer<std::uint8_t>(data[offset]);
    const std::uint32_t b =
        offset + 1U < size ? std::to_integer<std::uint8_t>(data[offset + 1U]) : 0;
    const std::uint32_t c =
        offset + 2U < size ? std::to_integer<std::uint8_t>(data[offset + 2U]) : 0;
    const std::uint32_t value = (a << 16U) | (b << 8U) | c;
    output.push_back(alphabet[(value >> 18U) & 0x3fU]);
    output.push_back(alphabet[(value >> 12U) & 0x3fU]);
    output.push_back(offset + 1U < size ? alphabet[(value >> 6U) & 0x3fU] : '=');
    output.push_back(offset + 2U < size ? alphabet[value & 0x3fU] : '=');
  }
  return output;
}

std::string Lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) { return std::tolower(character); });
  return result;
}

std::optional<std::string> Header(std::string_view request, std::string_view name) {
  const std::string target = Lower(name);
  std::size_t start = request.find("\r\n") + 2U;
  while (start < request.size()) {
    const std::size_t end = request.find("\r\n", start);
    if (end == std::string_view::npos || end == start) break;
    const std::size_t colon = request.find(':', start);
    if (colon != std::string_view::npos && colon < end &&
        Lower(request.substr(start, colon - start)) == target) {
      std::size_t valueStart = colon + 1U;
      while (valueStart < end && request[valueStart] == ' ') ++valueStart;
      return std::string(request.substr(valueStart, end - valueStart));
    }
    start = end + 2U;
  }
  return std::nullopt;
}

std::uint64_t ReadU64(const std::byte* source) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8U) | std::to_integer<std::uint8_t>(source[index]);
  }
  return value;
}

}  // namespace

bool BuildUpgradeResponse(std::string_view request, std::string* response) {
  const std::string connection = Lower(Header(request, "Connection").value_or(""));
  if (response == nullptr || request.size() > 16U * 1024U ||
      !request.starts_with("GET /direct HTTP/1.1\r\n") ||
      Lower(Header(request, "Upgrade").value_or("")) != "websocket" ||
      connection.find("upgrade") == std::string::npos ||
      Header(request, "Sec-WebSocket-Version") != "13") {
    return false;
  }
  const auto key = Header(request, "Sec-WebSocket-Key");
  if (!key || key->size() < 20U || key->size() > 64U) return false;
  const auto digest = Sha1(*key + std::string(kMagic));
  *response = "HTTP/1.1 101 Switching Protocols\r\n"
              "Upgrade: websocket\r\n"
              "Connection: Upgrade\r\n"
              "Sec-WebSocket-Accept: " +
              Base64(digest.data(), digest.size()) + "\r\n\r\n";
  return true;
}

std::vector<std::byte> EncodeFrame(Opcode opcode, std::string_view payload) {
  if (payload.size() > kMaxMessageBytes) return {};
  std::vector<std::byte> output;
  output.push_back(static_cast<std::byte>(0x80U | static_cast<std::uint8_t>(opcode)));
  if (payload.size() <= 125U) {
    output.push_back(static_cast<std::byte>(payload.size()));
  } else if (payload.size() <= 0xffffU) {
    output.push_back(static_cast<std::byte>(126));
    output.push_back(static_cast<std::byte>((payload.size() >> 8U) & 0xffU));
    output.push_back(static_cast<std::byte>(payload.size() & 0xffU));
  } else {
    output.push_back(static_cast<std::byte>(127));
    const std::uint64_t size = payload.size();
    for (int shift = 56; shift >= 0; shift -= 8) {
      output.push_back(static_cast<std::byte>((size >> shift) & 0xffU));
    }
  }
  const auto* begin = reinterpret_cast<const std::byte*>(payload.data());
  output.insert(output.end(), begin, begin + payload.size());
  return output;
}

bool Decoder::Push(const std::byte* data, std::size_t size,
                   std::vector<Message>* messages) {
  if (data == nullptr || messages == nullptr || buffer_.size() + size > kMaxMessageBytes * 2U) {
    Reset();
    return false;
  }
  buffer_.insert(buffer_.end(), data, data + size);
  while (buffer_.size() >= 2U) {
    const std::uint8_t first = std::to_integer<std::uint8_t>(buffer_[0]);
    const std::uint8_t second = std::to_integer<std::uint8_t>(buffer_[1]);
    const bool final = (first & 0x80U) != 0;
    const std::uint8_t opcodeValue = first & 0x0fU;
    if ((first & 0x70U) != 0 || (second & 0x80U) == 0) {
      Reset();
      return false;
    }
    std::uint64_t payloadLength = second & 0x7fU;
    std::size_t headerSize = 2U;
    if (payloadLength == 126U) {
      if (buffer_.size() < 4U) break;
      payloadLength = (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer_[2])) << 8U) |
                      std::to_integer<std::uint8_t>(buffer_[3]);
      headerSize = 4U;
    } else if (payloadLength == 127U) {
      if (buffer_.size() < 10U) break;
      payloadLength = ReadU64(buffer_.data() + 2U);
      headerSize = 10U;
    }
    const bool control = opcodeValue >= 0x8U;
    if (payloadLength > kMaxMessageBytes || (control && (!final || payloadLength > 125U))) {
      Reset();
      return false;
    }
    const std::size_t frameSize = headerSize + 4U + static_cast<std::size_t>(payloadLength);
    if (buffer_.size() < frameSize) break;
    const auto* mask = buffer_.data() + headerSize;
    const auto* payload = mask + 4U;
    std::vector<std::byte> decoded(static_cast<std::size_t>(payloadLength));
    for (std::size_t index = 0; index < decoded.size(); ++index) {
      decoded[index] = payload[index] ^ mask[index % 4U];
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));

    const auto opcode = static_cast<Opcode>(opcodeValue);
    if (opcode == Opcode::kText || opcode == Opcode::kBinary) {
      if (fragmented_opcode_) {
        Reset();
        return false;
      }
      if (final) {
        messages->push_back({opcode, std::move(decoded)});
      } else {
        fragmented_opcode_ = opcode;
        fragmented_ = std::move(decoded);
      }
    } else if (opcode == Opcode::kContinuation) {
      if (!fragmented_opcode_ || fragmented_.size() + decoded.size() > kMaxMessageBytes) {
        Reset();
        return false;
      }
      fragmented_.insert(fragmented_.end(), decoded.begin(), decoded.end());
      if (final) {
        messages->push_back({*fragmented_opcode_, std::move(fragmented_)});
        fragmented_opcode_.reset();
        fragmented_.clear();
      }
    } else if (opcode == Opcode::kClose || opcode == Opcode::kPing ||
               opcode == Opcode::kPong) {
      messages->push_back({opcode, std::move(decoded)});
    } else {
      Reset();
      return false;
    }
  }
  return true;
}

void Decoder::Reset() {
  buffer_.clear();
  fragmented_.clear();
  fragmented_opcode_.reset();
}

}  // namespace hss::receiver::websocket
