#include "network_gate.h"

#include <algorithm>
#include <iostream>

int main() {
  std::string error;
  const auto profiles = hss::host::NetworkGate::ConnectedWifiProfiles(&error);
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<std::wstring> allowedProfileIds;
  for (const auto& profile : profiles) {
    allowedProfileIds.push_back(profile.id);
  }
  const auto addresses =
      hss::host::NetworkGate::AllowedWifiIpv4Addresses(allowedProfileIds, &error);
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  for (const auto& profile : profiles) {
    if (profile.id.empty() || profile.name.empty() || profile.ipv4_addresses.empty()) {
      std::cerr << "Connected Wi-Fi profile metadata is incomplete\n";
      return 1;
    }
    for (const auto& address : profile.ipv4_addresses) {
      if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
        std::cerr << "Application-approved Wi-Fi address was omitted from the gate\n";
        return 1;
      }
    }
  }
  for (const auto& address : addresses) {
    if (!hss::host::NetworkGate::IsAllowedWifiIpv4(address, allowedProfileIds, &error)) {
      std::cerr << "Live approved Wi-Fi address failed revalidation: " << address << '\n';
      return 1;
    }
  }
  if (hss::host::NetworkGate::IsAllowedWifiIpv4("127.0.0.1", allowedProfileIds, &error)) {
    std::cerr << "Loopback must never pass the trusted Wi-Fi gate\n";
    return 1;
  }
  error.clear();
  if (!hss::host::NetworkGate::AllowedWifiIpv4Addresses({L"not-a-guid"}, &error).empty() ||
      error.empty()) {
    std::cerr << "Invalid application-approved Wi-Fi ID was not rejected\n";
    return 1;
  }
  std::cout << "Application-approved physical Wi-Fi revalidation contract passed\n";
  return 0;
}
