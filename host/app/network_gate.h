#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hss::host {

struct WifiNetworkProfile final {
  std::wstring id;
  std::wstring name;
  std::vector<std::string> ipv4_addresses;
};

class NetworkGate final {
 public:
  static std::vector<WifiNetworkProfile> ConnectedWifiProfiles(std::string* error);

  // Resolves only the physical Wi-Fi IPv4 addresses belonging to application-approved
  // Network List Manager profile IDs. Windows Public/Private category is intentionally ignored.
  static std::vector<std::string> AllowedWifiIpv4Addresses(
      const std::vector<std::wstring>& allowedProfileIds, std::string* error);
  static bool IsAllowedWifiIpv4(std::string_view address,
                                const std::vector<std::wstring>& allowedProfileIds,
                                std::string* error);
};

}  // namespace hss::host
