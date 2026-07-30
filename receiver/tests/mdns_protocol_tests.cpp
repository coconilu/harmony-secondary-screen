#include "mdns_protocol.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using hss::receiver::mdns::Ipv4Address;

std::uint8_t Byte(std::byte value) {
  return std::to_integer<std::uint8_t>(value);
}

std::uint32_t ReadU32(std::span<const std::byte> packet, std::size_t offset) {
  assert(offset + 4U <= packet.size());
  return (static_cast<std::uint32_t>(Byte(packet[offset])) << 24U) |
         (static_cast<std::uint32_t>(Byte(packet[offset + 1U])) << 16U) |
         (static_cast<std::uint32_t>(Byte(packet[offset + 2U])) << 8U) |
         Byte(packet[offset + 3U]);
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

std::vector<std::byte> CompressedAResponse(Ipv4Address address) {
  auto query = hss::receiver::mdns::BuildAQuery();
  std::vector<std::byte> response = query;
  response[2] = std::byte{0x84};
  response[3] = std::byte{0x00};
  response[7] = std::byte{0x01};
  response.push_back(std::byte{0xc0});
  response.push_back(std::byte{0x0c});
  AppendU16(&response, 1);
  AppendU16(&response, 0x8001);
  AppendU32(&response, hss::receiver::mdns::kRecordTtlSeconds);
  AppendU16(&response, 4);
  response.insert(response.end(), address.begin(), address.end());
  return response;
}

std::vector<std::byte> KnownAnswerQuery(Ipv4Address address,
                                        std::uint32_t ttlSeconds) {
  auto query = hss::receiver::mdns::BuildAQuery();
  query[7] = std::byte{1};
  const auto record = hss::receiver::mdns::BuildARecord(address, ttlSeconds);
  query.insert(query.end(), record.begin() + 12, record.end());
  return query;
}

void LegalQueryAndTtl() {
  const Ipv4Address address{std::byte{192}, std::byte{168}, std::byte{1},
                            std::byte{20}};
  const auto query = hss::receiver::mdns::BuildAQuery();
  const auto response = hss::receiver::mdns::BuildAResponse(query, address);
  assert(!response.empty());
  assert(response.size() <= hss::receiver::mdns::kMaximumResponseBytes);
  assert(response.size() * 2U <= query.size() * 3U);
  hss::receiver::mdns::ParsedName name;
  assert(hss::receiver::mdns::DecodeName(response, 12, &name));
  assert(name.value == hss::receiver::mdns::kHostname);
  const std::size_t record = 12U + name.consumed;
  assert(ReadU32(response, record + 4U) ==
         hss::receiver::mdns::kRecordTtlSeconds);
  const auto records = hss::receiver::mdns::ExtractARecords(response);
  assert(records.size() == 1U);
  assert(records.front() == address);
}

void AddressParsingAndTrustBoundary() {
  Ipv4Address address{};
  assert(hss::receiver::mdns::ParseIpv4Address("192.168.1.8", &address));
  assert(hss::receiver::mdns::IsTrustedLanAddress(address));
  assert(!hss::receiver::mdns::ParseIpv4Address("192.168.001.8", &address));
  assert(hss::receiver::mdns::ParseIpv4Address("8.8.8.8", &address));
  assert(!hss::receiver::mdns::IsTrustedLanAddress(address));
  assert(!hss::receiver::mdns::ParseIpv4Address("0.0.0.0.0", &address));
}

void CaseInsensitiveAndAnyQuery() {
  const Ipv4Address address{std::byte{10}, std::byte{1}, std::byte{2},
                            std::byte{3}};
  auto query = hss::receiver::mdns::BuildAQuery();
  for (std::size_t index = 12; index + 4U < query.size(); ++index) {
    if (Byte(query[index]) >= 'a' && Byte(query[index]) <= 'z') {
      query[index] = static_cast<std::byte>(Byte(query[index]) - 'a' + 'A');
    }
  }
  query[query.size() - 4U] = std::byte{0x00};
  query[query.size() - 3U] = std::byte{0xff};
  assert(!hss::receiver::mdns::BuildAResponse(query, address).empty());
}

void CompressedNameAndConflictRecords() {
  const Ipv4Address selected{std::byte{192}, std::byte{168}, std::byte{4},
                             std::byte{8}};
  const Ipv4Address conflicting{std::byte{192}, std::byte{168}, std::byte{4},
                                std::byte{9}};
  const auto same = hss::receiver::mdns::ExtractARecords(
      CompressedAResponse(selected));
  const auto different = hss::receiver::mdns::ExtractARecords(
      CompressedAResponse(conflicting));
  assert(same.size() == 1U && same.front() == selected);
  assert(different.size() == 1U && different.front() != selected);
}

void ProbeCarriesTheProposedAddressBeforePublication() {
  const Ipv4Address selected{std::byte{10}, std::byte{20}, std::byte{30},
                             std::byte{40}};
  const Ipv4Address conflicting{std::byte{10}, std::byte{20}, std::byte{30},
                                std::byte{41}};
  const auto selectedProbe = hss::receiver::mdns::BuildAProbe(selected);
  const auto conflictingProbe = hss::receiver::mdns::BuildAProbe(conflicting);
  assert(!selectedProbe.empty());
  assert(selectedProbe.size() <= hss::receiver::mdns::kMaximumResponseBytes);
  assert(hss::receiver::mdns::BuildAResponse(selectedProbe, selected).empty());
  assert(hss::receiver::mdns::ExtractARecords(selectedProbe) ==
         std::vector<Ipv4Address>{selected});
  assert(hss::receiver::mdns::ExtractARecords(conflictingProbe) ==
         std::vector<Ipv4Address>{conflicting});
}

void KnownAnswerSuppressionRefreshesOnlyExpiringRecords() {
  const Ipv4Address selected{std::byte{192}, std::byte{168}, std::byte{20},
                             std::byte{9}};
  const auto fresh = KnownAnswerQuery(
      selected, hss::receiver::mdns::kRecordTtlSeconds);
  const auto halfLife = KnownAnswerQuery(
      selected, hss::receiver::mdns::kRecordTtlSeconds / 2U);
  const auto expiring = KnownAnswerQuery(
      selected, hss::receiver::mdns::kRecordTtlSeconds / 2U - 1U);
  assert(hss::receiver::mdns::BuildAResponse(fresh, selected).empty());
  assert(hss::receiver::mdns::BuildAResponse(halfLife, selected).empty());
  assert(!hss::receiver::mdns::BuildAResponse(expiring, selected).empty());
}

void RejectsUnrelatedAndMalformedQueries() {
  const Ipv4Address address{std::byte{169}, std::byte{254}, std::byte{2},
                            std::byte{7}};
  auto unrelated = hss::receiver::mdns::BuildAQuery();
  unrelated[13] = std::byte{'x'};
  assert(hss::receiver::mdns::BuildAResponse(unrelated, address).empty());

  auto aaaa = hss::receiver::mdns::BuildAQuery();
  aaaa[aaaa.size() - 3U] = std::byte{28};
  assert(hss::receiver::mdns::BuildAResponse(aaaa, address).empty());

  auto truncated = hss::receiver::mdns::BuildAQuery();
  truncated.pop_back();
  assert(hss::receiver::mdns::BuildAResponse(truncated, address).empty());

  auto multiple = hss::receiver::mdns::BuildAQuery();
  multiple[5] = std::byte{2};
  assert(hss::receiver::mdns::BuildAResponse(multiple, address).empty());

  auto responseAsQuery = hss::receiver::mdns::BuildAQuery();
  responseAsQuery[2] = std::byte{0x80};
  assert(hss::receiver::mdns::BuildAResponse(responseAsQuery, address).empty());

  std::vector<std::byte> pointerLoop(18U, std::byte{0});
  pointerLoop[5] = std::byte{1};
  pointerLoop[12] = std::byte{0xc0};
  pointerLoop[13] = std::byte{0x0c};
  pointerLoop[15] = std::byte{1};
  pointerLoop[17] = std::byte{1};
  assert(hss::receiver::mdns::BuildAResponse(pointerLoop, address).empty());

  std::vector<std::byte> oversized(
      hss::receiver::mdns::kMaximumQueryBytes + 1U, std::byte{0});
  assert(hss::receiver::mdns::BuildAResponse(oversized, address).empty());
}

void GoodbyeRecordHasZeroTtl() {
  const Ipv4Address address{std::byte{172}, std::byte{20}, std::byte{0},
                            std::byte{5}};
  const auto goodbye = hss::receiver::mdns::BuildARecord(address, 0);
  hss::receiver::mdns::ParsedName name;
  assert(hss::receiver::mdns::DecodeName(goodbye, 12, &name));
  const std::size_t record = 12U + name.consumed;
  assert(ReadU32(goodbye, record + 4U) == 0U);
  assert(goodbye.size() <= hss::receiver::mdns::kMaximumResponseBytes);
}

void AddressChangeAndStopRevokeTheOldRecord() {
  const Ipv4Address oldAddress{std::byte{192}, std::byte{168}, std::byte{8},
                               std::byte{20}};
  const Ipv4Address newAddress{std::byte{192}, std::byte{168}, std::byte{8},
                               std::byte{21}};
  const auto oldAnnouncement = hss::receiver::mdns::BuildARecord(
      oldAddress, hss::receiver::mdns::kRecordTtlSeconds);
  const auto oldGoodbye = hss::receiver::mdns::BuildARecord(oldAddress, 0);
  const auto newAnnouncement = hss::receiver::mdns::BuildARecord(
      newAddress, hss::receiver::mdns::kRecordTtlSeconds);

  assert(hss::receiver::mdns::ExtractARecords(oldAnnouncement) ==
         std::vector<Ipv4Address>{oldAddress});
  assert(hss::receiver::mdns::ExtractARecords(oldGoodbye).empty());
  assert(hss::receiver::mdns::ExtractARecords(newAnnouncement) ==
         std::vector<Ipv4Address>{newAddress});

  hss::receiver::mdns::ParsedName oldName;
  assert(hss::receiver::mdns::DecodeName(oldGoodbye, 12, &oldName));
  const std::size_t oldRecord = 12U + oldName.consumed;
  assert(ReadU32(oldGoodbye, oldRecord + 4U) == 0U);
}

}  // namespace

int main() {
  LegalQueryAndTtl();
  AddressParsingAndTrustBoundary();
  CaseInsensitiveAndAnyQuery();
  CompressedNameAndConflictRecords();
  ProbeCarriesTheProposedAddressBeforePublication();
  KnownAnswerSuppressionRefreshesOnlyExpiringRecords();
  RejectsUnrelatedAndMalformedQueries();
  GoodbyeRecordHasZeroTtl();
  AddressChangeAndStopRevokeTheOldRecord();
  std::cout << "mDNS fixed A protocol tests passed\n";
  return 0;
}
