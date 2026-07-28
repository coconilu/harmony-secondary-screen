#include "mdns_responder.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace hss::receiver {
namespace {

constexpr int kMaximumInitialProbeDelayMilliseconds = 250;
constexpr int kProbeIntervalMilliseconds = 250;
constexpr int kProbeCount = 3;
constexpr int kReceiveSliceMilliseconds = 50;
constexpr int kPublishedReceiveMilliseconds = 100;
constexpr std::uint64_t kResponseWindowMilliseconds = 1000;

bool ContainsDifferentAddress(
    const std::vector<mdns::Ipv4Address>& addresses,
    mdns::Ipv4Address selectedAddress) {
  return std::any_of(addresses.begin(), addresses.end(),
                     [selectedAddress](mdns::Ipv4Address candidate) {
                       return candidate != selectedAddress;
                     });
}

}  // namespace

MdnsResponder::MdnsResponder(std::unique_ptr<MdnsTransport> transport)
    : transport_(std::move(transport)) {}

MdnsResponder::~MdnsResponder() {
  Stop();
}

bool MdnsResponder::Start(const std::string& addressText,
                          std::uint32_t interfaceIndex) {
  Stop();
  mdns::Ipv4Address address{};
  if (transport_ == nullptr ||
      !mdns::ParseIpv4Address(addressText, &address) ||
      !mdns::IsTrustedLanAddress(address) || interfaceIndex == 0U) {
    state_ = MdnsPublisherState::kError;
    return false;
  }
  std::scoped_lock lock(lifecycle_mutex_);
  selected_interface_ = {address, interfaceIndex};
  if (!transport_->Open(selected_interface_)) {
    state_ = MdnsPublisherState::kError;
    return false;
  }
  desired_ = true;
  goodbye_sent_ = false;
  response_window_initialized_ = false;
  response_window_started_ms_ = 0;
  responses_in_window_ = 0;
  state_ = MdnsPublisherState::kProbing;
  try {
    worker_ = std::thread(&MdnsResponder::Run, this);
  } catch (...) {
    desired_ = false;
    transport_->Close();
    state_ = MdnsPublisherState::kError;
    return false;
  }
  return true;
}

void MdnsResponder::Stop() {
  std::thread worker;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    if (state_.load() == MdnsPublisherState::kPublished) {
      SendGoodbyeLocked();
    }
    desired_ = false;
    if (transport_ != nullptr) transport_->Close();
    if (worker_.joinable()) worker = std::move(worker_);
  }
  if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
    worker.join();
  }
  state_ = MdnsPublisherState::kStopped;
}

void MdnsResponder::Run() {
  const int initialDelay = std::clamp(
      transport_->RandomDelayMilliseconds(
          kMaximumInitialProbeDelayMilliseconds),
      0, kMaximumInitialProbeDelayMilliseconds);
  if (!ObserveFor(initialDelay)) return;

  for (int probeIndex = 0; probeIndex < kProbeCount && desired_;
       ++probeIndex) {
    {
      std::scoped_lock lock(lifecycle_mutex_);
      if (!desired_) return;
      if (!SendProbeLocked()) {
        FailLocked();
        return;
      }
    }
    if (!ObserveFor(kProbeIntervalMilliseconds)) return;
  }
  {
    std::scoped_lock lock(lifecycle_mutex_);
    if (!desired_) return;
    if (!SendAnnouncementLocked()) {
      FailLocked();
      return;
    }
    state_ = MdnsPublisherState::kPublished;
  }

  while (desired_) {
    MdnsDatagram datagram;
    const auto result =
        transport_->Receive(kPublishedReceiveMilliseconds, &datagram);
    if (!desired_) break;
    if (result == MdnsReceiveResult::kTimeout) continue;
    if (result == MdnsReceiveResult::kError) {
      std::scoped_lock lock(lifecycle_mutex_);
      if (desired_) FailLocked();
      break;
    }
    if (!HandlePublishedDatagram(datagram)) break;
  }
}

bool MdnsResponder::ObserveFor(int milliseconds) {
  const std::uint64_t started = transport_->NowMilliseconds();
  const std::uint64_t deadline =
      started + static_cast<std::uint64_t>(std::max(0, milliseconds));
  while (desired_) {
    const std::uint64_t now = transport_->NowMilliseconds();
    if (now >= deadline) return true;
    const auto remaining = static_cast<int>(
        std::min<std::uint64_t>(deadline - now,
                                kReceiveSliceMilliseconds));
    MdnsDatagram datagram;
    const auto result = transport_->Receive(remaining, &datagram);
    if (!desired_) return false;
    if (result == MdnsReceiveResult::kTimeout) continue;
    if (result == MdnsReceiveResult::kError) {
      std::scoped_lock lock(lifecycle_mutex_);
      if (desired_) FailLocked();
      return false;
    }
    if (!HandleProbingDatagram(datagram)) return false;
  }
  return false;
}

bool MdnsResponder::AcceptDatagram(const MdnsDatagram& datagram) const {
  return datagram.bytes.size() <= mdns::kMaximumQueryBytes &&
         mdns::IsTrustedLanAddress(datagram.sourceAddress) &&
         datagram.sourcePort == mdns::kMulticastPort &&
         datagram.destinationAddress == mdns::kMulticastAddress &&
         datagram.interfaceIndex == selected_interface_.index &&
         datagram.hopLimit == mdns::kRequiredHopLimit;
}

bool MdnsResponder::HandleProbingDatagram(const MdnsDatagram& datagram) {
  if (!AcceptDatagram(datagram)) return true;
  const auto addresses = mdns::ExtractARecords(datagram.bytes);
  if (!ContainsDifferentAddress(addresses, selected_interface_.address)) {
    return true;
  }
  const auto kind = mdns::ClassifyMessage(datagram.bytes);
  if (kind == mdns::MessageKind::kProbe) {
    const bool losesTieBreak =
        std::any_of(addresses.begin(), addresses.end(),
                    [this](mdns::Ipv4Address candidate) {
                      return mdns::CompareAddresses(
                                 candidate, selected_interface_.address) > 0;
                    });
    std::scoped_lock lock(lifecycle_mutex_);
    if (!desired_) return false;
    if (losesTieBreak) {
      ConflictLocked(false);
      return false;
    }
    if (!SendProbeLocked()) {
      FailLocked();
      return false;
    }
    return true;
  }
  if (kind == mdns::MessageKind::kResponse ||
      kind == mdns::MessageKind::kQuery) {
    std::scoped_lock lock(lifecycle_mutex_);
    if (desired_) ConflictLocked(false);
    return false;
  }
  return true;
}

bool MdnsResponder::HandlePublishedDatagram(
    const MdnsDatagram& datagram) {
  if (!AcceptDatagram(datagram)) return true;
  const auto kind = mdns::ClassifyMessage(datagram.bytes);
  const auto addresses = mdns::ExtractARecords(datagram.bytes);
  if (ContainsDifferentAddress(addresses, selected_interface_.address)) {
    std::scoped_lock lock(lifecycle_mutex_);
    if (!desired_) return false;
    if (kind == mdns::MessageKind::kProbe) {
      if (!SendAnnouncementLocked()) {
        FailLocked();
        return false;
      }
      return true;
    }
    if (kind == mdns::MessageKind::kResponse ||
        kind == mdns::MessageKind::kQuery) {
      ConflictLocked(true);
      return false;
    }
  }
  const auto response =
      mdns::BuildAResponse(datagram.bytes, selected_interface_.address);
  if (response.empty() ||
      !AllowQueryResponse(transport_->NowMilliseconds())) {
    return true;
  }
  std::scoped_lock lock(lifecycle_mutex_);
  if (!desired_) return false;
  if (!transport_->Send(response)) {
    FailLocked();
    return false;
  }
  return true;
}

bool MdnsResponder::SendProbeLocked() {
  const auto probe = mdns::BuildAProbe(selected_interface_.address);
  return !probe.empty() && transport_->Send(probe);
}

bool MdnsResponder::SendAnnouncementLocked() {
  const auto announcement = mdns::BuildARecord(
      selected_interface_.address, mdns::kRecordTtlSeconds);
  return !announcement.empty() && transport_->Send(announcement);
}

void MdnsResponder::SendGoodbyeLocked() {
  if (goodbye_sent_ || transport_ == nullptr) return;
  goodbye_sent_ = true;
  const auto goodbye =
      mdns::BuildARecord(selected_interface_.address, 0);
  if (!goodbye.empty()) static_cast<void>(transport_->Send(goodbye));
}

void MdnsResponder::FailLocked() {
  state_ = MdnsPublisherState::kError;
  desired_ = false;
  transport_->Close();
}

void MdnsResponder::ConflictLocked(bool wasPublished) {
  if (wasPublished) SendGoodbyeLocked();
  state_ = MdnsPublisherState::kConflict;
  desired_ = false;
  transport_->Close();
}

bool MdnsResponder::AllowQueryResponse(std::uint64_t nowMilliseconds) {
  if (!response_window_initialized_ ||
      nowMilliseconds - response_window_started_ms_ >=
          kResponseWindowMilliseconds) {
    response_window_initialized_ = true;
    response_window_started_ms_ = nowMilliseconds;
    responses_in_window_ = 0;
  }
  if (responses_in_window_ >= mdns::kMaximumResponsesPerSecond) {
    return false;
  }
  ++responses_in_window_;
  return true;
}

}  // namespace hss::receiver
