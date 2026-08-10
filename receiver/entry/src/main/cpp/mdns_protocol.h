#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hss::receiver::mdns {

using Ipv4Address = std::array<std::byte, 4>;

inline constexpr char kHostname[] = "tabreach.local";
inline constexpr std::uint32_t kRecordTtlSeconds = 120;
inline constexpr std::size_t kMaximumQueryBytes = 512;
inline constexpr std::size_t kMaximumResponseBytes = 64;
inline constexpr Ipv4Address kMulticastAddress{
    std::byte{224}, std::byte{0}, std::byte{0}, std::byte{251}};
inline constexpr std::uint16_t kMulticastPort = 5353;
inline constexpr int kWindowsQueryHopLimit = 1;
inline constexpr int kRequiredResponseHopLimit = 255;
inline constexpr std::size_t kMaximumResponsesPerSecond = 10;

enum class MessageKind {
  kInvalid,
  kQuery,
  kProbe,
  kResponse,
};

struct ParsedName {
  std::string value;
  std::size_t consumed = 0;
};

bool DecodeName(std::span<const std::byte> packet, std::size_t offset, ParsedName* name);
bool ParseIpv4Address(std::string_view text, Ipv4Address* address);
bool IsTrustedLanAddress(Ipv4Address address);
int CompareAddresses(Ipv4Address left, Ipv4Address right);
MessageKind ClassifyMessage(std::span<const std::byte> packet);
std::vector<std::byte> BuildAQuery();
std::vector<std::byte> BuildAProbe(Ipv4Address address);
std::vector<std::byte> BuildARecord(Ipv4Address address, std::uint32_t ttlSeconds);
std::vector<std::byte> BuildAResponse(std::span<const std::byte> query,
                                      Ipv4Address address);
std::vector<Ipv4Address> ExtractARecords(std::span<const std::byte> packet);

}  // namespace hss::receiver::mdns
