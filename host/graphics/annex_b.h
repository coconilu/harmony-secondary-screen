#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace hss::graphics {

inline bool HasAnnexBNalType(std::span<const std::byte> bytes, std::uint8_t wanted) {
  for (std::size_t offset = 0; offset + 3 < bytes.size(); ++offset) {
    std::size_t prefix = 0;
    if (bytes[offset] == std::byte{0} && bytes[offset + 1] == std::byte{0} &&
        bytes[offset + 2] == std::byte{1}) {
      prefix = 3;
    } else if (offset + 4 < bytes.size() && bytes[offset] == std::byte{0} &&
               bytes[offset + 1] == std::byte{0} &&
               bytes[offset + 2] == std::byte{0} &&
               bytes[offset + 3] == std::byte{1}) {
      prefix = 4;
    }
    if (prefix != 0 &&
        (std::to_integer<std::uint8_t>(bytes[offset + prefix]) & 0x1fU) == wanted) {
      return true;
    }
  }
  return false;
}

inline bool ContainsAvcCodecConfig(std::span<const std::byte> bytes) {
  return HasAnnexBNalType(bytes, 7) && HasAnnexBNalType(bytes, 8);
}

}  // namespace hss::graphics
