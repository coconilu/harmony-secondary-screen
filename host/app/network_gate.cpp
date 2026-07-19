#include "network_gate.h"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace hss::host {
namespace {

using Microsoft::WRL::ComPtr;

std::string HResultMessage(const char* operation, HRESULT result) {
  std::ostringstream stream;
  stream << operation << " failed: 0x" << std::hex << static_cast<unsigned long>(result);
  return stream.str();
}

class ComApartment final {
 public:
  ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComApartment() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }
  HRESULT result() const { return result_; }

 private:
  HRESULT result_;
};

struct GuidLess {
  bool operator()(const GUID& lhs, const GUID& rhs) const {
    return std::memcmp(&lhs, &rhs, sizeof(GUID)) < 0;
  }
};

bool ParseAdapterGuid(const char* adapterName, GUID* guid) {
  if (adapterName == nullptr || guid == nullptr) {
    return false;
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, adapterName, -1, nullptr, 0);
  if (length <= 0) {
    return false;
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, adapterName, -1, wide.data(), length) == 0) {
    return false;
  }
  return SUCCEEDED(CLSIDFromString(wide.data(), guid));
}

std::wstring GuidString(const GUID& guid) {
  std::array<wchar_t, 40> buffer{};
  const int length = StringFromGUID2(guid, buffer.data(), static_cast<int>(buffer.size()));
  return length > 1 ? std::wstring(buffer.data(), static_cast<std::size_t>(length - 1)) : L"";
}

using WifiAdapterAddresses = std::map<GUID, std::vector<std::string>, GuidLess>;

WifiAdapterAddresses ConnectedWifiAdapterAddresses(std::string* error) {
  ULONG bytes = 16U * 1024U;
  std::vector<std::byte> storage(bytes);
  auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
  ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                   GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, addresses, &bytes);
  if (result == ERROR_BUFFER_OVERFLOW) {
    storage.resize(bytes);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                               GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, addresses, &bytes);
  }
  if (result != NO_ERROR) {
    *error = "GetAdaptersAddresses failed: " + std::to_string(result);
    return {};
  }

  WifiAdapterAddresses wifiAdapters;
  for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType != IF_TYPE_IEEE80211) {
      continue;
    }
    GUID adapterId{};
    if (!ParseAdapterGuid(adapter->AdapterName, &adapterId)) {
      continue;
    }
    auto& ipv4Addresses = wifiAdapters[adapterId];
    for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr == nullptr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      char buffer[INET_ADDRSTRLEN]{};
      const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), buffer,
                    static_cast<DWORD>(std::size(buffer))) != nullptr) {
        ipv4Addresses.emplace_back(buffer);
      }
    }
    std::ranges::sort(ipv4Addresses);
    ipv4Addresses.erase(std::unique(ipv4Addresses.begin(), ipv4Addresses.end()), ipv4Addresses.end());
  }
  return wifiAdapters;
}

struct NetworkConnections final {
  std::size_t wifi = 0;
  std::size_t other = 0;
  std::set<std::string> wifi_ipv4_addresses;
};

bool ReadNetworkConnections(INetwork* network, const WifiAdapterAddresses& wifiAdapters,
                            NetworkConnections* summary, std::string* error) {
  if (summary == nullptr) {
    *error = "Network connection summary is missing";
    return false;
  }
  ComPtr<IEnumNetworkConnections> connections;
  const HRESULT connectionsResult = network->GetNetworkConnections(&connections);
  if (FAILED(connectionsResult)) {
    *error = HResultMessage("INetwork::GetNetworkConnections", connectionsResult);
    return false;
  }
  for (;;) {
    ComPtr<INetworkConnection> connection;
    ULONG fetched = 0;
    const HRESULT result = connections->Next(1, &connection, &fetched);
    if (result == S_FALSE || fetched == 0) {
      break;
    }
    if (FAILED(result)) {
      *error = HResultMessage("IEnumNetworkConnections::Next", result);
      return false;
    }
    VARIANT_BOOL isConnected = VARIANT_FALSE;
    if (FAILED(connection->get_IsConnected(&isConnected))) {
      ++summary->other;
      continue;
    }
    if (isConnected != VARIANT_TRUE) {
      continue;
    }
    GUID adapterId{};
    if (FAILED(connection->GetAdapterId(&adapterId))) {
      ++summary->other;
      continue;
    }
    const auto adapter = wifiAdapters.find(adapterId);
    if (adapter == wifiAdapters.end()) {
      ++summary->other;
      continue;
    }
    ++summary->wifi;
    summary->wifi_ipv4_addresses.insert(adapter->second.begin(), adapter->second.end());
  }
  return true;
}

HRESULT ConnectedNetworks(ComPtr<IEnumNetworks>& networks, std::string* error) {
  ComPtr<INetworkListManager> manager;
  const HRESULT createResult = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                                                IID_PPV_ARGS(&manager));
  if (FAILED(createResult)) {
    *error = HResultMessage("CoCreateInstance(NetworkListManager)", createResult);
    return createResult;
  }
  const HRESULT networksResult =
      manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, networks.ReleaseAndGetAddressOf());
  if (FAILED(networksResult)) {
    *error = HResultMessage("GetNetworks", networksResult);
  }
  return networksResult;
}

}  // namespace

NetworkConnectionClass ClassifyNetworkConnections(std::size_t wifiConnections,
                                                   std::size_t otherConnections) {
  if (wifiConnections == 0) {
    return otherConnections == 0 ? NetworkConnectionClass::kNone
                                 : NetworkConnectionClass::kMixedOrUnknown;
  }
  return otherConnections == 0 ? NetworkConnectionClass::kWifiOnly
                               : NetworkConnectionClass::kMixedOrUnknown;
}

std::vector<WifiNetworkProfile> NetworkGate::ConnectedWifiProfiles(std::string* error) {
  if (error == nullptr) {
    return {};
  }
  error->clear();
  ComApartment apartment;
  if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE) {
    *error = HResultMessage("CoInitializeEx", apartment.result());
    return {};
  }

  const auto wifiAdapters = ConnectedWifiAdapterAddresses(error);
  if (!error->empty()) {
    return {};
  }

  ComPtr<IEnumNetworks> networks;
  if (FAILED(ConnectedNetworks(networks, error))) {
    return {};
  }

  std::vector<WifiNetworkProfile> profiles;
  for (;;) {
    ComPtr<INetwork> network;
    ULONG fetched = 0;
    const HRESULT nextResult = networks->Next(1, &network, &fetched);
    if (nextResult == S_FALSE || fetched == 0) {
      break;
    }
    if (FAILED(nextResult)) {
      *error = HResultMessage("IEnumNetworks::Next", nextResult);
      return {};
    }

    NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
    const HRESULT categoryResult = network->GetCategory(&category);
    if (FAILED(categoryResult)) {
      *error = HResultMessage("INetwork::GetCategory", categoryResult);
      return {};
    }

    NetworkConnections connectionSummary;
    if (!ReadNetworkConnections(network.Get(), wifiAdapters, &connectionSummary, error)) {
      return {};
    }
    if (connectionSummary.wifi == 0 || connectionSummary.wifi_ipv4_addresses.empty()) {
      continue;
    }

    GUID networkId{};
    const HRESULT idResult = network->GetNetworkId(&networkId);
    if (FAILED(idResult)) {
      *error = HResultMessage("INetwork::GetNetworkId", idResult);
      return {};
    }

    BSTR rawName = nullptr;
    std::wstring name = L"当前 Wi-Fi";
    const HRESULT nameResult = network->GetName(&rawName);
    if (SUCCEEDED(nameResult) && rawName != nullptr) {
      const std::wstring reportedName(rawName, SysStringLen(rawName));
      if (!reportedName.empty()) {
        name = reportedName;
      }
    }
    if (rawName != nullptr) {
      SysFreeString(rawName);
    }
    profiles.push_back(
        {GuidString(networkId), std::move(name), category == NLM_NETWORK_CATEGORY_PRIVATE,
         ClassifyNetworkConnections(connectionSummary.wifi, connectionSummary.other) ==
             NetworkConnectionClass::kWifiOnly,
         {connectionSummary.wifi_ipv4_addresses.begin(),
          connectionSummary.wifi_ipv4_addresses.end()}});
  }
  std::ranges::sort(profiles, [](const WifiNetworkProfile& lhs, const WifiNetworkProfile& rhs) {
    return lhs.id < rhs.id;
  });
  return profiles;
}

std::vector<std::string> NetworkGate::TrustedWifiIpv4Addresses(std::string* error) {
  const auto profiles = ConnectedWifiProfiles(error);
  if (error == nullptr || !error->empty()) {
    return {};
  }
  std::set<std::string> unique;
  for (const auto& profile : profiles) {
    if (profile.is_private) {
      unique.insert(profile.ipv4_addresses.begin(), profile.ipv4_addresses.end());
    }
  }
  return {unique.begin(), unique.end()};
}

bool NetworkGate::IsTrustedWifiIpv4(std::string_view address, std::string* error) {
  const auto addresses = TrustedWifiIpv4Addresses(error);
  return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
}

bool NetworkGate::TrustWifiProfile(std::wstring_view networkId, std::string* error) {
  if (error == nullptr) {
    return false;
  }
  error->clear();
  GUID requestedId{};
  const std::wstring idCopy(networkId);
  if (FAILED(CLSIDFromString(idCopy.c_str(), &requestedId))) {
    *error = "无效的 Wi-Fi 网络标识";
    return false;
  }

  ComApartment apartment;
  if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE) {
    *error = HResultMessage("CoInitializeEx", apartment.result());
    return false;
  }
  const auto wifiAdapters = ConnectedWifiAdapterAddresses(error);
  if (!error->empty()) {
    return false;
  }
  ComPtr<IEnumNetworks> networks;
  if (FAILED(ConnectedNetworks(networks, error))) {
    return false;
  }

  for (;;) {
    ComPtr<INetwork> network;
    ULONG fetched = 0;
    const HRESULT nextResult = networks->Next(1, &network, &fetched);
    if (nextResult == S_FALSE || fetched == 0) {
      break;
    }
    if (FAILED(nextResult)) {
      *error = HResultMessage("IEnumNetworks::Next", nextResult);
      return false;
    }
    GUID currentId{};
    if (FAILED(network->GetNetworkId(&currentId)) || !InlineIsEqualGUID(currentId, requestedId)) {
      continue;
    }
    NetworkConnections connectionSummary;
    if (!ReadNetworkConnections(network.Get(), wifiAdapters, &connectionSummary, error)) {
      return false;
    }
    if (ClassifyNetworkConnections(connectionSummary.wifi, connectionSummary.other) !=
        NetworkConnectionClass::kWifiOnly) {
      *error = "当前网络还聚合了 VPN、以太网或未知连接，不能安全地自动修改网络类别";
      return false;
    }
    if (connectionSummary.wifi_ipv4_addresses.empty()) {
      *error = "目标网络不是当前连接的物理 Wi-Fi";
      return false;
    }
    NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
    const HRESULT categoryResult = network->GetCategory(&category);
    if (FAILED(categoryResult)) {
      *error = HResultMessage("INetwork::GetCategory", categoryResult);
      return false;
    }
    if (category == NLM_NETWORK_CATEGORY_PRIVATE) {
      return true;
    }
    if (category != NLM_NETWORK_CATEGORY_PUBLIC) {
      *error = "只允许把当前公用 Wi-Fi 标记为可信网络";
      return false;
    }
    const HRESULT setResult = network->SetCategory(NLM_NETWORK_CATEGORY_PRIVATE);
    if (FAILED(setResult)) {
      *error = HResultMessage("INetwork::SetCategory", setResult);
      return false;
    }
    NLM_NETWORK_CATEGORY updatedCategory = NLM_NETWORK_CATEGORY_PUBLIC;
    const HRESULT verifyResult = network->GetCategory(&updatedCategory);
    if (FAILED(verifyResult) || updatedCategory != NLM_NETWORK_CATEGORY_PRIVATE) {
      *error = FAILED(verifyResult) ? HResultMessage("INetwork::GetCategory", verifyResult)
                                    : "Windows 未确认 Wi-Fi 网络类别变更";
      return false;
    }
    return true;
  }
  *error = "指定的物理 Wi-Fi 已断开或不存在";
  return false;
}

}  // namespace hss::host
