#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hss::host {

enum class NetworkConnectionClass {
  kNone,
  kWifiOnly,
  kMixedOrUnknown,
};

NetworkConnectionClass ClassifyNetworkConnections(std::size_t wifiConnections,
                                                   std::size_t otherConnections);

struct WifiNetworkProfile final {
  std::wstring id;
  std::wstring name;
  bool is_private = false;
  bool is_wifi_only = false;
  std::vector<std::string> ipv4_addresses;
};

class NetworkGate final {
 public:
  static std::vector<WifiNetworkProfile> ConnectedWifiProfiles(std::string* error);

  // Returns only physical Wi-Fi IPv4 addresses whose Windows network profile is Private.
  // An empty result is a hard refusal to listen.
  static std::vector<std::string> TrustedWifiIpv4Addresses(std::string* error);
  static bool IsTrustedWifiIpv4(std::string_view address, std::string* error);

  // Changes one currently connected physical Wi-Fi profile to Private. The caller must obtain
  // explicit user confirmation and elevation before calling this method.
  static bool TrustWifiProfile(std::wstring_view networkId, std::string* error);
};

}  // namespace hss::host
