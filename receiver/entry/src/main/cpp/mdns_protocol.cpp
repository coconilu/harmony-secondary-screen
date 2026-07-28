#include "mdns_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <utility>

namespace hss::receiver::mdns {
namespace {

constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kMaximumPointerJumps = 16;
constexpr std::size_t kMaximumRecords = 16;
constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypeAny = 255;
constexpr std::uint16_t kClassIn = 1;
constexpr std::uint16_t kCacheFlush = 0x8000;
constexpr std::uint16_t kQueryResponse = 0x8000;
constexpr std::uint16_t kAuthoritative = 0x0400;
constexpr std::uint16_t kTruncated = 0x0200;
constexpr std::uint16_t kOpcodeMask = 0x7800;

std::uint8_t Byte(std::byte value) {
  return std::to_integer<std::uint8_t>(value);
}

bool ReadU16(std::span<const std::byte> packet, std::size_t offset, std::uint16_t* value) {
  if (value == nullptr || offset > packet.size() || packet.size() - offset < 2U) {
    return false;
  }
  *value = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(Byte(packet[offset])) << 8U) |
      Byte(packet[offset + 1U]));
  return true;
}

bool ReadU32(std::span<const std::byte> packet, std::size_t offset, std::uint32_t* value) {
  if (value == nullptr || offset > packet.size() || packet.size() - offset < 4U) {
    return false;
  }
  *value = (static_cast<std::uint32_t>(Byte(packet[offset])) << 24U) |
           (static_cast<std::uint32_t>(Byte(packet[offset + 1U])) << 16U) |
           (static_cast<std::uint32_t>(Byte(packet[offset + 2U])) << 8U) |
           Byte(packet[offset + 3U]);
  return true;
}

void AppendU16(std::vector<std::byte>* packet, std::uint16_t value) {
  packet->push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
  packet->push_back(static_cast<std::byte>(value & 0xffU));
}

void AppendU32(std::vector<std::byte>* packet, std::uint32_t value) {
  packet->push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
  packet->push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
  packet->push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
  packet->push_back(static_cast<std::byte>(value & 0xffU));
}

bool ValidLabelCharacter(std::uint8_t value) {
  return std::isalnum(value) != 0 || value == '-';
}

std::vector<std::byte> EncodedHostname() {
  std::vector<std::byte> encoded;
  const std::string hostname(kHostname);
  std::size_t start = 0;
  while (start < hostname.size()) {
    const std::size_t end = hostname.find('.', start);
    const std::size_t length =
        (end == std::string::npos ? hostname.size() : end) - start;
    if (length == 0U || length > 63U) return {};
    encoded.push_back(static_cast<std::byte>(length));
    for (std::size_t index = 0; index < length; ++index) {
      encoded.push_back(static_cast<std::byte>(hostname[start + index]));
    }
    if (end == std::string::npos) break;
    start = end + 1U;
  }
  encoded.push_back(std::byte{0});
  return encoded;
}

bool SkipQuestions(std::span<const std::byte> packet, std::size_t* offset,
                   std::uint16_t count) {
  if (offset == nullptr) return false;
  for (std::uint16_t index = 0; index < count; ++index) {
    ParsedName name;
    if (!DecodeName(packet, *offset, &name)) return false;
    if (*offset > packet.size() || name.consumed > packet.size() - *offset) return false;
    *offset += name.consumed;
    if (*offset > packet.size() || packet.size() - *offset < 4U) return false;
    *offset += 4U;
  }
  return true;
}

}  // namespace

bool DecodeName(std::span<const std::byte> packet, std::size_t offset, ParsedName* name) {
  if (name == nullptr || offset >= packet.size()) return false;
  std::string output;
  std::size_t cursor = offset;
  std::size_t consumed = 0;
  bool jumped = false;
  std::array<std::size_t, kMaximumPointerJumps> visited{};
  std::size_t visitedCount = 0;

  while (cursor < packet.size()) {
    const std::uint8_t length = Byte(packet[cursor]);
    if ((length & 0xc0U) == 0xc0U) {
      if (cursor + 1U >= packet.size() || visitedCount >= visited.size()) return false;
      const std::size_t pointer =
          (static_cast<std::size_t>(length & 0x3fU) << 8U) |
          Byte(packet[cursor + 1U]);
      if (pointer >= packet.size() ||
          std::find(visited.begin(), visited.begin() + visitedCount, pointer) !=
              visited.begin() + visitedCount) {
        return false;
      }
      visited[visitedCount++] = pointer;
      if (!jumped) consumed += 2U;
      cursor = pointer;
      jumped = true;
      continue;
    }
    if ((length & 0xc0U) != 0U || length > 63U) return false;
    ++cursor;
    if (!jumped) ++consumed;
    if (length == 0U) {
      name->value = std::move(output);
      name->consumed = consumed;
      return !name->value.empty() && name->value.size() <= 253U;
    }
    if (cursor > packet.size() || length > packet.size() - cursor) return false;
    if (!output.empty()) output.push_back('.');
    for (std::size_t index = 0; index < length; ++index) {
      const std::uint8_t character = Byte(packet[cursor + index]);
      if (!ValidLabelCharacter(character)) return false;
      output.push_back(static_cast<char>(std::tolower(character)));
    }
    if (output.size() > 253U) return false;
    cursor += length;
    if (!jumped) consumed += length;
  }
  return false;
}

std::vector<std::byte> BuildAQuery() {
  std::vector<std::byte> query(kHeaderBytes, std::byte{0});
  query[5] = std::byte{1};
  const auto hostname = EncodedHostname();
  query.insert(query.end(), hostname.begin(), hostname.end());
  AppendU16(&query, kTypeA);
  AppendU16(&query, kClassIn);
  return query;
}

std::vector<std::byte> BuildAProbe(Ipv4Address address) {
  auto probe = BuildAQuery();
  if (probe.empty()) return {};
  probe[probe.size() - 4U] = std::byte{0x00};
  probe[probe.size() - 3U] = std::byte{0xff};
  probe[9] = std::byte{1};
  probe.push_back(std::byte{0xc0});
  probe.push_back(std::byte{0x0c});
  AppendU16(&probe, kTypeA);
  AppendU16(&probe, kClassIn);
  AppendU32(&probe, kRecordTtlSeconds);
  AppendU16(&probe, 4);
  probe.insert(probe.end(), address.begin(), address.end());
  if (probe.size() > kMaximumResponseBytes) return {};
  return probe;
}

std::vector<std::byte> BuildARecord(Ipv4Address address, std::uint32_t ttlSeconds) {
  std::vector<std::byte> response(kHeaderBytes, std::byte{0});
  response[2] = static_cast<std::byte>((kQueryResponse | kAuthoritative) >> 8U);
  response[3] = static_cast<std::byte>((kQueryResponse | kAuthoritative) & 0xffU);
  response[7] = std::byte{1};
  const auto hostname = EncodedHostname();
  response.insert(response.end(), hostname.begin(), hostname.end());
  AppendU16(&response, kTypeA);
  AppendU16(&response, kClassIn | kCacheFlush);
  AppendU32(&response, ttlSeconds);
  AppendU16(&response, 4);
  response.insert(response.end(), address.begin(), address.end());
  if (response.size() > kMaximumResponseBytes) return {};
  return response;
}

std::vector<std::byte> BuildAResponse(std::span<const std::byte> query,
                                      Ipv4Address address) {
  if (query.size() < kHeaderBytes || query.size() > kMaximumQueryBytes) return {};
  std::uint16_t identifier = 0;
  std::uint16_t flags = 0;
  std::uint16_t questionCount = 0;
  std::uint16_t answerCount = 0;
  std::uint16_t authorityCount = 0;
  std::uint16_t additionalCount = 0;
  if (!ReadU16(query, 0, &identifier) || !ReadU16(query, 2, &flags) ||
      !ReadU16(query, 4, &questionCount) || !ReadU16(query, 6, &answerCount) ||
      !ReadU16(query, 8, &authorityCount) ||
      !ReadU16(query, 10, &additionalCount)) {
    return {};
  }
  if (identifier != 0U || (flags & (kQueryResponse | kOpcodeMask | kTruncated)) != 0U ||
      questionCount != 1U || answerCount > kMaximumRecords ||
      authorityCount != 0U || additionalCount != 0U) {
    return {};
  }
  ParsedName name;
  if (!DecodeName(query, kHeaderBytes, &name) || name.value != kHostname) return {};
  std::size_t offset = kHeaderBytes + name.consumed;
  std::uint16_t type = 0;
  std::uint16_t recordClass = 0;
  if (!ReadU16(query, offset, &type) ||
      !ReadU16(query, offset + 2U, &recordClass)) {
    return {};
  }
  if ((type != kTypeA && type != kTypeAny) ||
      (recordClass & ~kCacheFlush) != kClassIn) {
    return {};
  }
  offset += 4U;
  bool suppressKnownAnswer = false;
  for (std::uint16_t index = 0; index < answerCount; ++index) {
    ParsedName answerName;
    if (!DecodeName(query, offset, &answerName) ||
        offset > query.size() ||
        answerName.consumed > query.size() - offset) {
      return {};
    }
    offset += answerName.consumed;
    std::uint16_t answerType = 0;
    std::uint16_t answerClass = 0;
    std::uint32_t answerTtl = 0;
    std::uint16_t answerLength = 0;
    if (!ReadU16(query, offset, &answerType) ||
        !ReadU16(query, offset + 2U, &answerClass) ||
        !ReadU32(query, offset + 4U, &answerTtl) ||
        !ReadU16(query, offset + 8U, &answerLength) ||
        answerName.value != kHostname || answerType != kTypeA ||
        (answerClass & ~kCacheFlush) != kClassIn || answerLength != 4U) {
      return {};
    }
    offset += 10U;
    if (offset > query.size() || answerLength > query.size() - offset) {
      return {};
    }
    const bool sameAddress =
        std::equal(address.begin(), address.end(), query.begin() + offset);
    suppressKnownAnswer =
        suppressKnownAnswer ||
        (sameAddress && answerTtl > kRecordTtlSeconds / 2U);
    offset += answerLength;
  }
  if (offset != query.size() || suppressKnownAnswer) return {};
  return BuildARecord(address, kRecordTtlSeconds);
}

std::vector<Ipv4Address> ExtractARecords(std::span<const std::byte> packet) {
  std::vector<Ipv4Address> addresses;
  if (packet.size() < kHeaderBytes || packet.size() > kMaximumQueryBytes) {
    return addresses;
  }
  std::uint16_t flags = 0;
  std::uint16_t questionCount = 0;
  std::uint16_t answerCount = 0;
  std::uint16_t authorityCount = 0;
  std::uint16_t additionalCount = 0;
  if (!ReadU16(packet, 2, &flags) || !ReadU16(packet, 4, &questionCount) ||
      !ReadU16(packet, 6, &answerCount) || !ReadU16(packet, 8, &authorityCount) ||
      !ReadU16(packet, 10, &additionalCount) ||
      (flags & (kOpcodeMask | kTruncated)) != 0U) {
    return addresses;
  }
  const bool response = (flags & kQueryResponse) != 0U;
  const bool knownAnswerQuery =
      !response && answerCount > 0U && authorityCount == 0U &&
      additionalCount == 0U;
  const bool probe =
      !response && answerCount == 0U && authorityCount > 0U &&
      additionalCount == 0U;
  if ((!response && !knownAnswerQuery && !probe) ||
      (response && answerCount == 0U && authorityCount == 0U &&
       additionalCount == 0U)) {
    return addresses;
  }
  const std::size_t recordCount =
      static_cast<std::size_t>(answerCount) + authorityCount + additionalCount;
  if (recordCount > kMaximumRecords) return addresses;
  std::size_t offset = kHeaderBytes;
  if (!SkipQuestions(packet, &offset, questionCount)) return {};
  for (std::size_t index = 0; index < recordCount; ++index) {
    ParsedName name;
    if (!DecodeName(packet, offset, &name)) return {};
    if (offset > packet.size() || name.consumed > packet.size() - offset) return {};
    offset += name.consumed;
    std::uint16_t type = 0;
    std::uint16_t recordClass = 0;
    std::uint16_t dataLength = 0;
    std::uint32_t ttl = 0;
    if (!ReadU16(packet, offset, &type) ||
        !ReadU16(packet, offset + 2U, &recordClass) ||
        !ReadU32(packet, offset + 4U, &ttl) ||
        !ReadU16(packet, offset + 8U, &dataLength)) {
      return {};
    }
    offset += 10U;
    if (offset > packet.size() || dataLength > packet.size() - offset) return {};
    if (ttl > 0U && name.value == kHostname && type == kTypeA &&
        (recordClass & ~kCacheFlush) == kClassIn && dataLength == 4U) {
      Ipv4Address address{};
      std::copy_n(packet.data() + offset, address.size(), address.begin());
      addresses.push_back(address);
    }
    offset += dataLength;
  }
  if (offset != packet.size()) return {};
  return addresses;
}

}  // namespace hss::receiver::mdns
