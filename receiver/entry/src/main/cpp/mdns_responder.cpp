#include "mdns_responder.h"

#include "mdns_protocol.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace hss::receiver {
namespace {

constexpr char kMulticastAddress[] = "224.0.0.251";
constexpr std::uint16_t kMulticastPort = 5353;
constexpr int kMulticastTtl = 255;
constexpr int kProbeWindowMilliseconds = 500;

bool TrustedLanSource(in_addr address) {
  const std::uint32_t hostOrder = ntohl(address.s_addr);
  const std::uint8_t first = static_cast<std::uint8_t>(hostOrder >> 24U);
  const std::uint8_t second = static_cast<std::uint8_t>((hostOrder >> 16U) & 0xffU);
  return first == 10U ||
         (first == 172U && second >= 16U && second <= 31U) ||
         (first == 192U && second == 168U) ||
         (first == 169U && second == 254U);
}

mdns::Ipv4Address ProtocolAddress(in_addr address) {
  mdns::Ipv4Address bytes{};
  std::memcpy(bytes.data(), &address.s_addr, bytes.size());
  return bytes;
}

bool ContainsDifferentAddress(const std::vector<mdns::Ipv4Address>& addresses,
                              in_addr selectedAddress) {
  const auto selected = ProtocolAddress(selectedAddress);
  return std::any_of(addresses.begin(), addresses.end(),
                     [&selected](const mdns::Ipv4Address& candidate) {
                       return candidate != selected;
                     });
}

}  // namespace

MdnsResponder::~MdnsResponder() {
  Stop();
}

bool MdnsResponder::Start(const std::string& addressText) {
  Stop();
  in_addr address{};
  if (inet_pton(AF_INET, addressText.c_str(), &address) != 1 ||
      !TrustedLanSource(address)) {
    state_ = MdnsPublisherState::kError;
    return false;
  }
  std::scoped_lock lock(lifecycle_mutex_);
  const int descriptor = OpenSocket(address);
  if (descriptor < 0) {
    state_ = MdnsPublisherState::kError;
    return false;
  }
  socket_ = descriptor;
  published_address_ = ProtocolAddress(address);
  desired_ = true;
  state_ = MdnsPublisherState::kProbing;
  try {
    worker_ = std::thread(&MdnsResponder::Run, this, address);
  } catch (...) {
    desired_ = false;
    CloseSocket();
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
      static_cast<void>(
          SendPacket(mdns::BuildARecord(published_address_, 0)));
    }
    desired_ = false;
    CloseSocket();
    if (worker_.joinable()) worker = std::move(worker_);
  }
  if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
    worker.join();
  }
  state_ = MdnsPublisherState::kStopped;
}

int MdnsResponder::OpenSocket(in_addr address) {
  const int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (descriptor < 0) return -1;
  int reuse = 1;
  if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    close(descriptor);
    return -1;
  }
#ifdef SO_REUSEPORT
  if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
    close(descriptor);
    return -1;
  }
#endif
  sockaddr_in multicast{};
  multicast.sin_family = AF_INET;
  multicast.sin_port = htons(kMulticastPort);
  if (inet_pton(AF_INET, kMulticastAddress, &multicast.sin_addr) != 1 ||
      bind(descriptor, reinterpret_cast<sockaddr*>(&multicast), sizeof(multicast)) != 0) {
    close(descriptor);
    return -1;
  }
  ip_mreq membership{};
  membership.imr_multiaddr = multicast.sin_addr;
  membership.imr_interface = address;
  if (setsockopt(descriptor, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                 sizeof(membership)) != 0 ||
      setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_IF, &address,
                 sizeof(address)) != 0 ||
      setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_TTL, &kMulticastTtl,
                 sizeof(kMulticastTtl)) != 0) {
    close(descriptor);
    return -1;
  }
  unsigned char loopback = 1;
  if (setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback,
                 sizeof(loopback)) != 0) {
    close(descriptor);
    return -1;
  }
  return descriptor;
}

void MdnsResponder::Run(in_addr address) {
  if (!SendPacket(mdns::BuildAProbe(ProtocolAddress(address)))) {
    state_ = MdnsPublisherState::kError;
    desired_ = false;
    CloseSocket();
    return;
  }
  const auto probeDeadline = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(kProbeWindowMilliseconds);
  while (desired_ && std::chrono::steady_clock::now() < probeDeadline) {
    std::vector<std::byte> packet;
    in_addr source{};
    if (!ReceivePacket(&packet, &source, 50)) continue;
    if (TrustedLanSource(source) &&
        ContainsDifferentAddress(mdns::ExtractARecords(packet), address)) {
      state_ = MdnsPublisherState::kConflict;
      desired_ = false;
      CloseSocket();
      return;
    }
  }
  if (!desired_) return;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    if (!desired_) return;
    if (!SendPacket(mdns::BuildARecord(ProtocolAddress(address),
                                       mdns::kRecordTtlSeconds))) {
      state_ = MdnsPublisherState::kError;
      desired_ = false;
      CloseSocket();
      return;
    }
    state_ = MdnsPublisherState::kPublished;
  }

  while (desired_) {
    std::vector<std::byte> packet;
    in_addr source{};
    if (!ReceivePacket(&packet, &source, 100) || !TrustedLanSource(source)) continue;
    if (ContainsDifferentAddress(mdns::ExtractARecords(packet), address)) {
      std::scoped_lock lock(lifecycle_mutex_);
      if (desired_) {
        static_cast<void>(
            SendPacket(mdns::BuildARecord(ProtocolAddress(address), 0)));
        state_ = MdnsPublisherState::kConflict;
        desired_ = false;
      }
      break;
    }
    const auto response = mdns::BuildAResponse(packet, ProtocolAddress(address));
    if (!response.empty()) {
      std::scoped_lock lock(lifecycle_mutex_);
      if (desired_ && !SendPacket(response)) {
        state_ = MdnsPublisherState::kError;
        desired_ = false;
        break;
      }
    }
  }
  CloseSocket();
}

bool MdnsResponder::SendPacket(const std::vector<std::byte>& packet) {
  const int descriptor = socket_.load();
  if (descriptor < 0 || packet.empty() ||
      packet.size() > mdns::kMaximumResponseBytes) {
    return false;
  }
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(kMulticastPort);
  if (inet_pton(AF_INET, kMulticastAddress, &destination.sin_addr) != 1) {
    return false;
  }
  const ssize_t sent =
      sendto(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL,
             reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
  return sent == static_cast<ssize_t>(packet.size());
}

bool MdnsResponder::ReceivePacket(std::vector<std::byte>* packet, in_addr* source,
                                  int timeoutMilliseconds) {
  if (packet == nullptr || source == nullptr || timeoutMilliseconds < 0) return false;
  const int descriptor = socket_.load();
  if (descriptor < 0) return false;
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(descriptor, &readSet);
  timeval timeout{timeoutMilliseconds / 1000,
                  (timeoutMilliseconds % 1000) * 1000};
  const int ready = select(descriptor + 1, &readSet, nullptr, nullptr, &timeout);
  if (ready <= 0 || !desired_) return false;
  std::array<std::byte, mdns::kMaximumQueryBytes + 1U> buffer{};
  sockaddr_in peer{};
  socklen_t peerLength = sizeof(peer);
  const ssize_t count =
      recvfrom(descriptor, buffer.data(), buffer.size(), 0,
               reinterpret_cast<sockaddr*>(&peer), &peerLength);
  if (count <= 0 || static_cast<std::size_t>(count) > mdns::kMaximumQueryBytes ||
      peer.sin_family != AF_INET) {
    return false;
  }
  packet->assign(buffer.begin(), buffer.begin() + count);
  *source = peer.sin_addr;
  return true;
}

void MdnsResponder::CloseSocket() {
  const int descriptor = socket_.exchange(-1);
  if (descriptor >= 0) {
    shutdown(descriptor, SHUT_RDWR);
    close(descriptor);
  }
}

}  // namespace hss::receiver
