#include "network_gate.h"

#include <algorithm>
#include <iostream>

int main() {
  using hss::host::ClassifyNetworkConnections;
  using hss::host::NetworkConnectionClass;
  if (ClassifyNetworkConnections(0, 0) != NetworkConnectionClass::kNone ||
      ClassifyNetworkConnections(1, 0) != NetworkConnectionClass::kWifiOnly ||
      ClassifyNetworkConnections(2, 0) != NetworkConnectionClass::kWifiOnly ||
      ClassifyNetworkConnections(1, 1) != NetworkConnectionClass::kMixedOrUnknown ||
      ClassifyNetworkConnections(0, 1) != NetworkConnectionClass::kMixedOrUnknown) {
    std::cerr << "Wi-Fi-only network connection policy failed\n";
    return 1;
  }

  std::string error;
  const auto profiles = hss::host::NetworkGate::ConnectedWifiProfiles(&error);
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto addresses = hss::host::NetworkGate::TrustedWifiIpv4Addresses(&error);
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  for (const auto& profile : profiles) {
    if (profile.id.empty() || profile.name.empty() || profile.ipv4_addresses.empty()) {
      std::cerr << "Connected Wi-Fi profile metadata is incomplete\n";
      return 1;
    }
    if (!profile.is_private) {
      continue;
    }
    for (const auto& address : profile.ipv4_addresses) {
      if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
        std::cerr << "Trusted Wi-Fi profile address was omitted from the gate\n";
        return 1;
      }
    }
  }
  for (const auto& address : addresses) {
    if (!hss::host::NetworkGate::IsTrustedWifiIpv4(address, &error)) {
      std::cerr << "Live trusted Wi-Fi address failed revalidation: " << address << '\n';
      return 1;
    }
  }
  if (hss::host::NetworkGate::IsTrustedWifiIpv4("127.0.0.1", &error)) {
    std::cerr << "Loopback must never pass the trusted Wi-Fi gate\n";
    return 1;
  }
  std::cout << "Live physical Wi-Fi revalidation contract passed\n";
  return 0;
}
