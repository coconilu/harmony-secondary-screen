#pragma once

#include <string>
#include <vector>

namespace hss::host {

class NetworkGate final {
 public:
  // Returns only IPv4 addresses attached to Windows networks categorized as Private.
  // An empty result is a hard refusal to listen.
  static std::vector<std::string> PrivateIpv4Addresses(std::string* error);
};

}  // namespace hss::host
