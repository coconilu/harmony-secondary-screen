#pragma once

#include "mdns_protocol.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hss::receiver {

struct MdnsInterface {
  mdns::Ipv4Address address{};
  std::uint32_t index = 0;
};

struct MdnsDatagram {
  std::vector<std::byte> bytes;
  mdns::Ipv4Address sourceAddress{};
  mdns::Ipv4Address destinationAddress{};
  std::uint16_t sourcePort = 0;
  std::uint32_t interfaceIndex = 0;
  int hopLimit = -1;
};

enum class MdnsReceiveResult {
  kTimeout,
  kPacket,
  kError,
};

class MdnsTransport {
 public:
  virtual ~MdnsTransport() = default;

  virtual bool Open(MdnsInterface selectedInterface) = 0;
  virtual void Close() = 0;
  virtual bool Send(std::span<const std::byte> packet) = 0;
  virtual MdnsReceiveResult Receive(int timeoutMilliseconds,
                                    MdnsDatagram* datagram) = 0;
  virtual int RandomDelayMilliseconds(int maximumInclusive) = 0;
  virtual std::uint64_t NowMilliseconds() const = 0;
};

std::unique_ptr<MdnsTransport> CreatePosixMdnsTransport();

}  // namespace hss::receiver
