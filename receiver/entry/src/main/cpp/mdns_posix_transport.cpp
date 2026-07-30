#include "mdns_responder.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>

namespace hss::receiver {
namespace {

in_addr NativeAddress(mdns::Ipv4Address address) {
  in_addr native{};
  std::memcpy(&native.s_addr, address.data(), address.size());
  return native;
}

mdns::Ipv4Address ProtocolAddress(in_addr address) {
  mdns::Ipv4Address protocol{};
  std::memcpy(protocol.data(), &address.s_addr, protocol.size());
  return protocol;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
cmsghdr* NextControlHeader(msghdr* message, cmsghdr* header) {
  return CMSG_NXTHDR(message, header);
}
#pragma clang diagnostic pop

class PosixMdnsTransport final : public MdnsTransport {
 public:
  ~PosixMdnsTransport() override { Close(); }

  bool Open(MdnsInterface selectedInterface) override {
    Close();
    const int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (descriptor < 0) return false;
    const auto fail = [descriptor]() {
      close(descriptor);
      return false;
    };
    int enabled = 1;
    int disabled = 0;
    if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled)) != 0 ||
        setsockopt(descriptor, SOL_SOCKET, SO_REUSEPORT, &enabled,
                   sizeof(enabled)) != 0 ||
        setsockopt(descriptor, IPPROTO_IP, IP_PKTINFO, &enabled,
                   sizeof(enabled)) != 0 ||
        setsockopt(descriptor, IPPROTO_IP, IP_RECVTTL, &enabled,
                   sizeof(enabled)) != 0 ||
        setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_ALL, &disabled,
                   sizeof(disabled)) != 0) {
      return fail();
    }

    sockaddr_in multicast{};
    multicast.sin_family = AF_INET;
    multicast.sin_port = htons(mdns::kMulticastPort);
    multicast.sin_addr = NativeAddress(mdns::kMulticastAddress);
    if (bind(descriptor, reinterpret_cast<sockaddr*>(&multicast),
             sizeof(multicast)) != 0) {
      return fail();
    }

    ip_mreqn membership{};
    membership.imr_multiaddr = multicast.sin_addr;
    membership.imr_address = NativeAddress(selectedInterface.address);
    membership.imr_ifindex = static_cast<int>(selectedInterface.index);
    int multicastTtl = mdns::kRequiredResponseHopLimit;
    unsigned char loopback = 1;
    if (setsockopt(descriptor, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                   sizeof(membership)) != 0 ||
        setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_IF, &membership,
                   sizeof(membership)) != 0 ||
        setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_TTL, &multicastTtl,
                   sizeof(multicastTtl)) != 0 ||
        setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback,
                   sizeof(loopback)) != 0) {
      return fail();
    }
    socket_ = descriptor;
    return true;
  }

  void Close() override {
    const int descriptor = socket_.exchange(-1);
    if (descriptor >= 0) {
      shutdown(descriptor, SHUT_RDWR);
      close(descriptor);
    }
  }

  bool Send(std::span<const std::byte> packet) override {
    const int descriptor = socket_.load();
    if (descriptor < 0 || packet.empty() ||
        packet.size() > mdns::kMaximumResponseBytes) {
      return false;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(mdns::kMulticastPort);
    destination.sin_addr = NativeAddress(mdns::kMulticastAddress);
    const ssize_t sent =
        sendto(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL,
               reinterpret_cast<sockaddr*>(&destination),
               sizeof(destination));
    return sent == static_cast<ssize_t>(packet.size());
  }

  MdnsReceiveResult Receive(int timeoutMilliseconds,
                            MdnsDatagram* datagram) override {
    if (datagram == nullptr || timeoutMilliseconds < 0) {
      return MdnsReceiveResult::kError;
    }
    const int descriptor = socket_.load();
    if (descriptor < 0) return MdnsReceiveResult::kTimeout;
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(descriptor, &readSet);
    timeval timeout{timeoutMilliseconds / 1000,
                    (timeoutMilliseconds % 1000) * 1000};
    const int ready =
        select(descriptor + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready == 0 || socket_.load() < 0) return MdnsReceiveResult::kTimeout;
    if (ready < 0) return MdnsReceiveResult::kError;

    std::array<std::byte, mdns::kMaximumQueryBytes + 1U> buffer{};
    std::array<unsigned char, 128> control{};
    sockaddr_in peer{};
    iovec vector{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_name = &peer;
    message.msg_namelen = sizeof(peer);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const ssize_t count = recvmsg(descriptor, &message, 0);
    if (count <= 0) {
      return socket_.load() < 0 ? MdnsReceiveResult::kTimeout
                                : MdnsReceiveResult::kError;
    }
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        static_cast<std::size_t>(count) > mdns::kMaximumQueryBytes ||
        peer.sin_family != AF_INET) {
      *datagram = {};
      return MdnsReceiveResult::kPacket;
    }

    in_pktinfo packetInfo{};
    bool hasPacketInfo = false;
    int hopLimit = -1;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = NextControlHeader(&message, header)) {
      if (header->cmsg_level != IPPROTO_IP) continue;
      if (header->cmsg_type == IP_PKTINFO &&
          header->cmsg_len >= CMSG_LEN(sizeof(packetInfo))) {
        std::memcpy(&packetInfo, CMSG_DATA(header), sizeof(packetInfo));
        hasPacketInfo = true;
      } else if (header->cmsg_type == IP_TTL &&
                 header->cmsg_len >= CMSG_LEN(sizeof(hopLimit))) {
        std::memcpy(&hopLimit, CMSG_DATA(header), sizeof(hopLimit));
      }
    }

    datagram->bytes.assign(buffer.begin(), buffer.begin() + count);
    datagram->sourceAddress = ProtocolAddress(peer.sin_addr);
    datagram->sourcePort = ntohs(peer.sin_port);
    datagram->destinationAddress =
        hasPacketInfo ? ProtocolAddress(packetInfo.ipi_addr)
                      : mdns::Ipv4Address{};
    datagram->interfaceIndex =
        hasPacketInfo && packetInfo.ipi_ifindex > 0
            ? static_cast<std::uint32_t>(packetInfo.ipi_ifindex)
            : 0U;
    datagram->hopLimit = hopLimit;
    return MdnsReceiveResult::kPacket;
  }

  int RandomDelayMilliseconds(int maximumInclusive) override {
    if (maximumInclusive <= 0) return 0;
    std::uint32_t value = 0;
    const int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random >= 0) {
      const ssize_t count = read(random, &value, sizeof(value));
      close(random);
      if (count == static_cast<ssize_t>(sizeof(value))) {
        return static_cast<int>(
            value % (static_cast<std::uint32_t>(maximumInclusive) + 1U));
      }
    }
    return maximumInclusive / 2;
  }

  std::uint64_t NowMilliseconds() const override {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

 private:
  std::atomic<int> socket_{-1};
};

}  // namespace

std::unique_ptr<MdnsTransport> CreatePosixMdnsTransport() {
  return std::make_unique<PosixMdnsTransport>();
}

MdnsResponder::MdnsResponder()
    : MdnsResponder(CreatePosixMdnsTransport()) {}

}  // namespace hss::receiver
