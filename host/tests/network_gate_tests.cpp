#include "network_gate.h"

#include <iostream>

int main() {
  std::string error;
  const auto addresses = hss::host::NetworkGate::PrivateIpv4Addresses(&error);
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  for (const auto& address : addresses) {
    if (!hss::host::NetworkGate::IsPrivateIpv4(address, &error)) {
      std::cerr << "Live private address failed revalidation: " << address << '\n';
      return 1;
    }
  }
  if (hss::host::NetworkGate::IsPrivateIpv4("127.0.0.1", &error)) {
    std::cerr << "Loopback must never pass the private network gate\n";
    return 1;
  }
  std::cout << "Live network revalidation contract passed\n";
  return 0;
}
