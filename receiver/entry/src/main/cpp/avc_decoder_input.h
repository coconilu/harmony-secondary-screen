#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hss::receiver {

struct AvcRecoveryInput final {
  std::vector<std::byte> codecData;
  std::vector<std::byte> syncFrame;
  bool hasSps = false;
  bool hasPps = false;
  bool hasIdr = false;

  bool complete() const { return hasSps && hasPps && hasIdr; }
};

inline std::size_t AvcStartCodeLength(std::span<const std::byte> bytes,
                                      std::size_t offset) {
  if (offset + 3 <= bytes.size() && bytes[offset] == std::byte{0} &&
      bytes[offset + 1] == std::byte{0} && bytes[offset + 2] == std::byte{1}) {
    return 3;
  }
  if (offset + 4 <= bytes.size() && bytes[offset] == std::byte{0} &&
      bytes[offset + 1] == std::byte{0} && bytes[offset + 2] == std::byte{0} &&
      bytes[offset + 3] == std::byte{1}) {
    return 4;
  }
  return 0;
}

inline std::size_t FindAvcStartCode(std::span<const std::byte> bytes,
                                    std::size_t offset) {
  for (std::size_t cursor = offset; cursor + 3 <= bytes.size(); ++cursor) {
    if (AvcStartCodeLength(bytes, cursor) != 0) return cursor;
  }
  return bytes.size();
}

inline AvcRecoveryInput SplitAvcRecoveryInput(std::span<const std::byte> bytes) {
  AvcRecoveryInput result;
  std::size_t start = FindAvcStartCode(bytes, 0);
  if (start != 0) return result;
  while (start < bytes.size()) {
    const std::size_t prefix = AvcStartCodeLength(bytes, start);
    const std::size_t nalOffset = start + prefix;
    if (prefix == 0 || nalOffset >= bytes.size()) return {};
    const std::size_t next = FindAvcStartCode(bytes, nalOffset + 1);
    const auto nalType = std::to_integer<std::uint8_t>(bytes[nalOffset]) & 0x1fU;
    auto& destination = nalType == 7 || nalType == 8 ? result.codecData
                                                     : result.syncFrame;
    destination.insert(destination.end(), bytes.begin() + static_cast<std::ptrdiff_t>(start),
                       bytes.begin() + static_cast<std::ptrdiff_t>(next));
    result.hasSps = result.hasSps || nalType == 7;
    result.hasPps = result.hasPps || nalType == 8;
    result.hasIdr = result.hasIdr || nalType == 5;
    start = next;
  }
  return result;
}

}  // namespace hss::receiver
