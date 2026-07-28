#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hss::receiver::mdns {

using Ipv4Address = std::array<std::byte, 4>;

inline constexpr char kHostname[] = "harmony-web-companion.local";
inline constexpr std::uint32_t kRecordTtlSeconds = 120;
inline constexpr std::size_t kMaximumQueryBytes = 512;
inline constexpr std::size_t kMaximumResponseBytes = 64;

struct ParsedName {
  std::string value;
  std::size_t consumed = 0;
};

bool DecodeName(std::span<const std::byte> packet, std::size_t offset, ParsedName* name);
std::vector<std::byte> BuildAQuery();
std::vector<std::byte> BuildAProbe(Ipv4Address address);
std::vector<std::byte> BuildARecord(Ipv4Address address, std::uint32_t ttlSeconds);
std::vector<std::byte> BuildAResponse(std::span<const std::byte> query,
                                      Ipv4Address address);
std::vector<Ipv4Address> ExtractARecords(std::span<const std::byte> packet);

}  // namespace hss::receiver::mdns
