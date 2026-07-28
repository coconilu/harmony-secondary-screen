#include "mdns_responder.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace {

using hss::receiver::MdnsDatagram;
using hss::receiver::MdnsInterface;
using hss::receiver::MdnsPublisherState;
using hss::receiver::MdnsReceiveResult;
using hss::receiver::MdnsResponder;
using hss::receiver::MdnsTransport;
using hss::receiver::mdns::Ipv4Address;

class FakeMdnsTransport;

class FakeMdnsBus {
 public:
  void Register(FakeMdnsTransport* transport);
  void Unregister(FakeMdnsTransport* transport);
  void Broadcast(FakeMdnsTransport* sender,
                 std::span<const std::byte> packet);
  std::size_t maximumParticipants() const {
    std::scoped_lock lock(mutex_);
    return maximum_participants_;
  }
  std::size_t currentParticipants() const {
    std::scoped_lock lock(mutex_);
    return participants_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<FakeMdnsTransport*> participants_;
  std::size_t maximum_participants_ = 0;
};

class FakeMdnsTransport final : public MdnsTransport {
 public:
  explicit FakeMdnsTransport(FakeMdnsBus* bus) : bus_(bus) {}
  ~FakeMdnsTransport() override { Close(); }

  bool Open(MdnsInterface selectedInterface) override {
    {
      std::scoped_lock lock(mutex_);
      selected_interface_ = selectedInterface;
      open_ = true;
      queue_.clear();
      ++open_count_;
    }
    bus_->Register(this);
    return true;
  }

  void Close() override {
    bool unregister = false;
    {
      std::scoped_lock lock(mutex_);
      unregister = open_;
      open_ = false;
      if (unregister) ++close_count_;
    }
    if (unregister) bus_->Unregister(this);
    condition_.notify_all();
  }

  bool Send(std::span<const std::byte> packet) override {
    {
      std::scoped_lock lock(mutex_);
      if (!open_) return false;
      sent_.emplace_back(packet.begin(), packet.end());
    }
    bus_->Broadcast(this, packet);
    return true;
  }

  MdnsReceiveResult Receive(int timeoutMilliseconds,
                            MdnsDatagram* datagram) override {
    if (datagram == nullptr || timeoutMilliseconds < 0) {
      return MdnsReceiveResult::kError;
    }
    std::unique_lock lock(mutex_);
    if (queue_.empty() && open_) {
      condition_.wait_for(
          lock, std::chrono::milliseconds(std::min(timeoutMilliseconds, 5)),
          [this]() { return !queue_.empty() || !open_; });
    }
    if (!queue_.empty()) {
      *datagram = std::move(queue_.front());
      queue_.pop_front();
      return MdnsReceiveResult::kPacket;
    }
    now_milliseconds_ += static_cast<std::uint64_t>(timeoutMilliseconds);
    return MdnsReceiveResult::kTimeout;
  }

  int RandomDelayMilliseconds(int maximumInclusive) override {
    return std::clamp(random_delay_milliseconds_, 0, maximumInclusive);
  }

  std::uint64_t NowMilliseconds() const override {
    std::scoped_lock lock(mutex_);
    return now_milliseconds_;
  }

  void Enqueue(MdnsDatagram datagram) {
    {
      std::scoped_lock lock(mutex_);
      if (!open_) return;
      queue_.push_back(std::move(datagram));
    }
    condition_.notify_all();
  }

  void Inject(std::vector<std::byte> bytes, Ipv4Address source,
              std::uint32_t interfaceIndex, std::uint16_t sourcePort,
              int hopLimit,
              Ipv4Address destination =
                  hss::receiver::mdns::kMulticastAddress) {
    Enqueue({std::move(bytes), source, destination, sourcePort,
             interfaceIndex, hopLimit});
  }

  MdnsInterface selectedInterface() const {
    std::scoped_lock lock(mutex_);
    return selected_interface_;
  }

  std::vector<std::vector<std::byte>> sentPackets() const {
    std::scoped_lock lock(mutex_);
    return sent_;
  }

  int openCount() const {
    std::scoped_lock lock(mutex_);
    return open_count_;
  }

  int closeCount() const {
    std::scoped_lock lock(mutex_);
    return close_count_;
  }

  void setRandomDelay(int milliseconds) {
    random_delay_milliseconds_ = milliseconds;
  }

 private:
  FakeMdnsBus* bus_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool open_ = false;
  MdnsInterface selected_interface_{};
  std::deque<MdnsDatagram> queue_;
  std::vector<std::vector<std::byte>> sent_;
  std::uint64_t now_milliseconds_ = 0;
  int random_delay_milliseconds_ = 0;
  int open_count_ = 0;
  int close_count_ = 0;
};

void FakeMdnsBus::Register(FakeMdnsTransport* transport) {
  std::scoped_lock lock(mutex_);
  if (std::find(participants_.begin(), participants_.end(), transport) ==
      participants_.end()) {
    participants_.push_back(transport);
  }
  maximum_participants_ =
      std::max(maximum_participants_, participants_.size());
}

void FakeMdnsBus::Unregister(FakeMdnsTransport* transport) {
  std::scoped_lock lock(mutex_);
  participants_.erase(
      std::remove(participants_.begin(), participants_.end(), transport),
      participants_.end());
}

void FakeMdnsBus::Broadcast(FakeMdnsTransport* sender,
                            std::span<const std::byte> packet) {
  std::vector<FakeMdnsTransport*> recipients;
  {
    std::scoped_lock lock(mutex_);
    recipients = participants_;
  }
  const MdnsInterface source = sender->selectedInterface();
  for (FakeMdnsTransport* recipient : recipients) {
    const MdnsInterface target = recipient->selectedInterface();
    if (target.index != source.index) continue;
    recipient->Enqueue({
        std::vector<std::byte>(packet.begin(), packet.end()),
        source.address,
        hss::receiver::mdns::kMulticastAddress,
        hss::receiver::mdns::kMulticastPort,
        target.index,
        hss::receiver::mdns::kRequiredResponseHopLimit,
    });
  }
}

bool WaitForState(const MdnsResponder& responder, MdnsPublisherState state,
                  int timeoutMilliseconds = 2500) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMilliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (responder.state() == state) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return responder.state() == state;
}

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

std::uint32_t RecordTtl(const std::vector<std::byte>& packet) {
  hss::receiver::mdns::ParsedName name;
  assert(hss::receiver::mdns::DecodeName(packet, 12, &name));
  return ReadU32(packet, 12U + name.consumed + 4U);
}

std::size_t CountGoodbyes(const FakeMdnsTransport& transport) {
  const auto packets = transport.sentPackets();
  return static_cast<std::size_t>(std::count_if(
      packets.begin(), packets.end(), [](const auto& packet) {
        return hss::receiver::mdns::ClassifyMessage(packet) ==
                   hss::receiver::mdns::MessageKind::kResponse &&
               RecordTtl(packet) == 0U;
      }));
}

std::size_t CountAnnouncements(const FakeMdnsTransport& transport) {
  const auto packets = transport.sentPackets();
  return static_cast<std::size_t>(std::count_if(
      packets.begin(), packets.end(), [](const auto& packet) {
        return hss::receiver::mdns::ClassifyMessage(packet) ==
                   hss::receiver::mdns::MessageKind::kResponse &&
               RecordTtl(packet) ==
                   hss::receiver::mdns::kRecordTtlSeconds;
      }));
}

std::vector<std::byte> KnownAnswerQuery(Ipv4Address address,
                                        std::uint32_t ttlSeconds) {
  auto query = hss::receiver::mdns::BuildAQuery();
  query[7] = std::byte{1};
  const auto record = hss::receiver::mdns::BuildARecord(address, ttlSeconds);
  query.insert(query.end(), record.begin() + 12, record.end());
  return query;
}

std::size_t CountPackets(const FakeMdnsTransport& transport,
                         const std::vector<std::byte>& expected) {
  const auto packets = transport.sentPackets();
  return static_cast<std::size_t>(
      std::count(packets.begin(), packets.end(), expected));
}

bool WaitForPacketCount(const FakeMdnsTransport& transport,
                        const std::vector<std::byte>& expected,
                        std::size_t minimumCount,
                        int timeoutMilliseconds = 500) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMilliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (CountPackets(transport, expected) >= minimumCount) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return CountPackets(transport, expected) >= minimumCount;
}

void InvalidAddressNeverOpensTransport() {
  FakeMdnsBus bus;
  auto transport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* evidence = transport.get();
  MdnsResponder responder(std::move(transport));
  assert(!responder.Start("8.8.8.8", 7));
  assert(responder.state() == MdnsPublisherState::kError);
  assert(evidence->openCount() == 0);
  assert(!responder.Start("192.168.1.8", 0));
  assert(evidence->openCount() == 0);
}

void AddressInvalidationSendsExactlyOneGoodbye() {
  FakeMdnsBus bus;
  auto transport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* evidence = transport.get();
  MdnsResponder responder(std::move(transport));
  assert(responder.Start("192.168.1.8", 7));
  assert(WaitForState(responder, MdnsPublisherState::kPublished));
  std::thread first([&responder]() { responder.Stop(); });
  std::thread second([&responder]() { responder.Stop(); });
  first.join();
  second.join();
  assert(responder.state() == MdnsPublisherState::kStopped);
  assert(CountGoodbyes(*evidence) == 1U);
  const Ipv4Address selected{std::byte{192}, std::byte{168}, std::byte{1},
                             std::byte{8}};
  const auto expectedGoodbye =
      hss::receiver::mdns::BuildARecord(selected, 0);
  const auto packets = evidence->sentPackets();
  assert(std::count(packets.begin(), packets.end(), expectedGoodbye) == 1);
  responder.Stop();
  assert(CountGoodbyes(*evidence) == 1U);
  assert(evidence->closeCount() == 1);
}

void OldKnownAnswerDoesNotConflictWhileProbing() {
  FakeMdnsBus bus;
  auto transport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* evidence = transport.get();
  evidence->setRandomDelay(250);
  MdnsResponder responder(std::move(transport));
  const Ipv4Address selected{std::byte{192}, std::byte{168}, std::byte{1},
                             std::byte{8}};
  const Ipv4Address oldAddress{std::byte{192}, std::byte{168}, std::byte{1},
                               std::byte{7}};
  assert(responder.Start("192.168.1.8", 7));
  assert(responder.state() == MdnsPublisherState::kProbing);
  evidence->Inject(
      KnownAnswerQuery(oldAddress, hss::receiver::mdns::kRecordTtlSeconds),
      oldAddress, 7, hss::receiver::mdns::kMulticastPort,
      hss::receiver::mdns::kRequiredResponseHopLimit);
  assert(WaitForState(responder, MdnsPublisherState::kPublished));
  assert(CountGoodbyes(*evidence) == 0U);
  assert(CountAnnouncements(*evidence) >= 1U);
  assert(CountPackets(
             *evidence,
             hss::receiver::mdns::BuildARecord(
                 selected, hss::receiver::mdns::kRecordTtlSeconds)) >= 1U);
  responder.Stop();
}

void OldKnownAnswerReturnsCurrentAddressWhilePublished() {
  FakeMdnsBus bus;
  auto transport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* evidence = transport.get();
  MdnsResponder responder(std::move(transport));
  const Ipv4Address selected{std::byte{192}, std::byte{168}, std::byte{1},
                             std::byte{8}};
  const Ipv4Address oldAddress{std::byte{192}, std::byte{168}, std::byte{1},
                               std::byte{7}};
  const auto currentRecord = hss::receiver::mdns::BuildARecord(
      selected, hss::receiver::mdns::kRecordTtlSeconds);
  assert(responder.Start("192.168.1.8", 7));
  assert(WaitForState(responder, MdnsPublisherState::kPublished));
  const std::size_t currentRecordsBefore =
      CountPackets(*evidence, currentRecord);
  evidence->Inject(
      KnownAnswerQuery(oldAddress, hss::receiver::mdns::kRecordTtlSeconds),
      oldAddress, 7, hss::receiver::mdns::kMulticastPort,
      hss::receiver::mdns::kRequiredResponseHopLimit);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline &&
         CountPackets(*evidence, currentRecord) == currentRecordsBefore) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  assert(responder.state() == MdnsPublisherState::kPublished);
  assert(CountGoodbyes(*evidence) == 0U);
  assert(CountPackets(*evidence, currentRecord) == currentRecordsBefore + 1U);
  responder.Stop();
}

void QueryAndProbeHopLimitsKeepAllOtherIngressGates() {
  FakeMdnsBus bus;
  auto transport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* evidence = transport.get();
  MdnsResponder responder(std::move(transport));
  const Ipv4Address selected{std::byte{192}, std::byte{168}, std::byte{1},
                             std::byte{8}};
  const Ipv4Address client{std::byte{192}, std::byte{168}, std::byte{1},
                           std::byte{40}};
  const Ipv4Address vpnClient{std::byte{10}, std::byte{20}, std::byte{30},
                              std::byte{40}};
  const Ipv4Address publicClient{std::byte{8}, std::byte{8}, std::byte{8},
                                 std::byte{8}};
  const auto query = hss::receiver::mdns::BuildAQuery();
  const auto currentRecord = hss::receiver::mdns::BuildARecord(
      selected, hss::receiver::mdns::kRecordTtlSeconds);
  assert(responder.Start("192.168.1.8", 7));
  assert(WaitForState(responder, MdnsPublisherState::kPublished));
  const std::size_t before = CountPackets(*evidence, currentRecord);

  evidence->Inject(query, vpnClient, 9,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kWindowsQueryHopLimit);
  evidence->Inject(query, client, 7, 9999,
                   hss::receiver::mdns::kWindowsQueryHopLimit);
  evidence->Inject(query, client, 7,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kWindowsQueryHopLimit, selected);
  evidence->Inject(query, publicClient, 7,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kWindowsQueryHopLimit);
  for (const int rejectedHopLimit : {2, 64, 128}) {
    evidence->Inject(query, client, 7,
                     hss::receiver::mdns::kMulticastPort,
                     rejectedHopLimit);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  assert(responder.state() == MdnsPublisherState::kPublished);
  assert(CountPackets(*evidence, currentRecord) == before);
  assert(CountGoodbyes(*evidence) == 0U);

  evidence->Inject(query, client, 7,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kWindowsQueryHopLimit);
  assert(WaitForPacketCount(*evidence, currentRecord, before + 1U));
  evidence->Inject(query, client, 7,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kRequiredResponseHopLimit);
  assert(WaitForPacketCount(*evidence, currentRecord, before + 2U));
  const Ipv4Address probeAddress{std::byte{192}, std::byte{168},
                                 std::byte{1}, std::byte{41}};
  evidence->Inject(hss::receiver::mdns::BuildAProbe(probeAddress),
                   probeAddress, 7, hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kWindowsQueryHopLimit);
  assert(WaitForPacketCount(*evidence, currentRecord, before + 3U));
  assert(responder.state() == MdnsPublisherState::kPublished);
  assert(CountGoodbyes(*evidence) == 0U);
  responder.Stop();
}

void WrongInterfacePortHopAndDestinationAreIgnored() {
  FakeMdnsBus bus;
  auto transport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* evidence = transport.get();
  MdnsResponder responder(std::move(transport));
  const Ipv4Address selected{std::byte{192}, std::byte{168}, std::byte{1},
                             std::byte{8}};
  const Ipv4Address conflict{std::byte{192}, std::byte{168}, std::byte{1},
                             std::byte{9}};
  assert(responder.Start("192.168.1.8", 7));
  assert(WaitForState(responder, MdnsPublisherState::kPublished));
  const auto record = hss::receiver::mdns::BuildARecord(
      conflict, hss::receiver::mdns::kRecordTtlSeconds);
  evidence->Inject(record, conflict, 9,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kRequiredResponseHopLimit);
  evidence->Inject(record, conflict, 7, 9999,
                   hss::receiver::mdns::kRequiredResponseHopLimit);
  evidence->Inject(record, conflict, 7,
                   hss::receiver::mdns::kMulticastPort, 64);
  evidence->Inject(record, conflict, 7,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kRequiredResponseHopLimit,
                   selected);
  evidence->Inject(record, conflict, 7,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kWindowsQueryHopLimit);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  assert(responder.state() == MdnsPublisherState::kPublished);
  assert(CountGoodbyes(*evidence) == 0U);
  evidence->Inject(record, conflict, 7,
                   hss::receiver::mdns::kMulticastPort,
                   hss::receiver::mdns::kRequiredResponseHopLimit);
  assert(WaitForState(responder, MdnsPublisherState::kConflict));
  assert(CountGoodbyes(*evidence) == 1U);
}

void EstablishedOwnerDefeatsALaterStarter() {
  FakeMdnsBus bus;
  auto ownerTransport = std::make_unique<FakeMdnsTransport>(&bus);
  auto newcomerTransport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* ownerEvidence = ownerTransport.get();
  FakeMdnsTransport* newcomerEvidence = newcomerTransport.get();
  MdnsResponder owner(std::move(ownerTransport));
  MdnsResponder newcomer(std::move(newcomerTransport));
  assert(owner.Start("192.168.1.8", 7));
  assert(WaitForState(owner, MdnsPublisherState::kPublished));
  const std::size_t announcementsBefore = CountAnnouncements(*ownerEvidence);
  assert(newcomer.Start("192.168.1.9", 7));
  assert(WaitForState(newcomer, MdnsPublisherState::kConflict));
  assert(owner.state() == MdnsPublisherState::kPublished);
  assert(CountAnnouncements(*ownerEvidence) > announcementsBefore);
  assert(CountAnnouncements(*newcomerEvidence) == 0U);
  assert(CountGoodbyes(*newcomerEvidence) == 0U);
  assert(bus.maximumParticipants() >= 2U);
  owner.Stop();
  assert(CountGoodbyes(*ownerEvidence) == 1U);
}

void SimultaneousProbesUseDeterministicTieBreak() {
  FakeMdnsBus bus;
  auto lowerTransport = std::make_unique<FakeMdnsTransport>(&bus);
  auto higherTransport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* lowerEvidence = lowerTransport.get();
  FakeMdnsTransport* higherEvidence = higherTransport.get();
  MdnsResponder lower(std::move(lowerTransport));
  MdnsResponder higher(std::move(higherTransport));
  assert(lower.Start("192.168.1.8", 7));
  assert(higher.Start("192.168.1.9", 7));
  assert(WaitForState(lower, MdnsPublisherState::kConflict));
  assert(WaitForState(higher, MdnsPublisherState::kPublished));
  assert(CountAnnouncements(*lowerEvidence) == 0U);
  assert(CountAnnouncements(*higherEvidence) >= 1U);
  assert(bus.maximumParticipants() >= 2U);
  higher.Stop();
}

void QueryResponsesRespectFrequencyBudget() {
  FakeMdnsBus bus;
  auto transport = std::make_unique<FakeMdnsTransport>(&bus);
  FakeMdnsTransport* evidence = transport.get();
  MdnsResponder responder(std::move(transport));
  const Ipv4Address client{std::byte{192}, std::byte{168}, std::byte{1},
                           std::byte{40}};
  assert(responder.Start("192.168.1.8", 7));
  assert(WaitForState(responder, MdnsPublisherState::kPublished));
  const std::size_t before = evidence->sentPackets().size();
  const auto query = hss::receiver::mdns::BuildAQuery();
  for (std::size_t index = 0;
       index < hss::receiver::mdns::kMaximumResponsesPerSecond + 5U;
       ++index) {
    evidence->Inject(query, client, 7,
                     hss::receiver::mdns::kMulticastPort,
                     hss::receiver::mdns::kRequiredResponseHopLimit);
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline &&
         evidence->sentPackets().size() <
             before + hss::receiver::mdns::kMaximumResponsesPerSecond) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  assert(evidence->sentPackets().size() ==
         before + hss::receiver::mdns::kMaximumResponsesPerSecond);
  responder.Stop();
}

void DestructorStopsAResponderBlockedInReceive() {
  FakeMdnsBus bus;
  auto transport = std::make_unique<FakeMdnsTransport>(&bus);
  {
    MdnsResponder responder(std::move(transport));
    assert(responder.Start("192.168.1.8", 7));
    assert(WaitForState(responder, MdnsPublisherState::kPublished));
    assert(bus.currentParticipants() == 1U);
  }
  assert(bus.currentParticipants() == 0U);
}

}  // namespace

int main() {
  InvalidAddressNeverOpensTransport();
  AddressInvalidationSendsExactlyOneGoodbye();
  OldKnownAnswerDoesNotConflictWhileProbing();
  OldKnownAnswerReturnsCurrentAddressWhilePublished();
  QueryAndProbeHopLimitsKeepAllOtherIngressGates();
  WrongInterfacePortHopAndDestinationAreIgnored();
  EstablishedOwnerDefeatsALaterStarter();
  SimultaneousProbesUseDeterministicTieBreak();
  QueryResponsesRespectFrequencyBudget();
  DestructorStopsAResponderBlockedInReceive();
  std::cout << "mDNS responder transport and ownership tests passed\n";
  return 0;
}
