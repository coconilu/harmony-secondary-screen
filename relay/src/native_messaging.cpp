#include "native_messaging.h"

#include <fcntl.h>
#include <io.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace hwc::relay {

void ConfigureNativeMessagingStdio() {
  if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
      _setmode(_fileno(stdout), _O_BINARY) == -1) {
    throw std::runtime_error("failed to configure native messaging stdio");
  }
}

std::optional<std::string> ReadNativeMessage() {
  std::array<std::uint8_t, 4> length_bytes{};
  const std::size_t read_count =
      std::fread(length_bytes.data(), 1, length_bytes.size(), stdin);
  if (read_count == 0 && std::feof(stdin) != 0) {
    return std::nullopt;
  }
  if (read_count != length_bytes.size()) {
    throw std::runtime_error("truncated native messaging length");
  }

  const std::uint32_t length =
      static_cast<std::uint32_t>(length_bytes[0]) |
      (static_cast<std::uint32_t>(length_bytes[1]) << 8U) |
      (static_cast<std::uint32_t>(length_bytes[2]) << 16U) |
      (static_cast<std::uint32_t>(length_bytes[3]) << 24U);
  if (length == 0 || length > kMaxNativeMessageBytes) {
    throw std::runtime_error("native messaging payload length is invalid");
  }

  std::string message(length, '\0');
  if (std::fread(message.data(), 1, length, stdin) != length) {
    throw std::runtime_error("truncated native messaging payload");
  }
  return message;
}

void WriteNativeMessage(const std::string_view message) {
  if (message.empty() ||
      message.size() > kMaxNativeMessageBytes ||
      message.size() >
          static_cast<std::size_t>(
              std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("native messaging output length is invalid");
  }

  const std::uint32_t length = static_cast<std::uint32_t>(message.size());
  const std::array<std::uint8_t, 4> length_bytes{
      static_cast<std::uint8_t>(length),
      static_cast<std::uint8_t>(length >> 8U),
      static_cast<std::uint8_t>(length >> 16U),
      static_cast<std::uint8_t>(length >> 24U)};
  if (std::fwrite(length_bytes.data(), 1, length_bytes.size(), stdout) !=
          length_bytes.size() ||
      std::fwrite(message.data(), 1, message.size(), stdout) !=
          message.size() ||
      std::fflush(stdout) != 0) {
    throw std::runtime_error("failed to write native messaging output");
  }
}

}  // namespace hwc::relay
