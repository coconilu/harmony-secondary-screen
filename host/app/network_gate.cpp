#include "network_gate.h"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>
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

std::set<GUID, GuidLess> PrivateAdapterIds(std::string* error) {
  std::set<GUID, GuidLess> ids;

  ComPtr<INetworkListManager> manager;
  const HRESULT createResult = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                                                IID_PPV_ARGS(&manager));
  if (FAILED(createResult)) {
    *error = HResultMessage("CoCreateInstance(NetworkListManager)", createResult);
    return ids;
  }

  ComPtr<IEnumNetworks> networks;
  const HRESULT networksResult = manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks);
  if (FAILED(networksResult)) {
    *error = HResultMessage("GetNetworks", networksResult);
    return ids;
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
      return {};
    }

    NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
    if (FAILED(network->GetCategory(&category)) || category != NLM_NETWORK_CATEGORY_PRIVATE) {
      continue;
    }

    ComPtr<IEnumNetworkConnections> connections;
    if (FAILED(network->GetNetworkConnections(&connections))) {
      continue;
    }
    for (;;) {
      ComPtr<INetworkConnection> connection;
      fetched = 0;
      const HRESULT connectionResult = connections->Next(1, &connection, &fetched);
      if (connectionResult == S_FALSE || fetched == 0) {
        break;
      }
      if (FAILED(connectionResult)) {
        break;
      }
      GUID adapterId{};
      if (SUCCEEDED(connection->GetAdapterId(&adapterId))) {
        ids.insert(adapterId);
      }
    }
  }
  return ids;
}

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

}  // namespace

std::vector<std::string> NetworkGate::PrivateIpv4Addresses(std::string* error) {
  if (error == nullptr) {
    return {};
  }
  error->clear();
  ComApartment apartment;
  if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE) {
    *error = HResultMessage("CoInitializeEx", apartment.result());
    return {};
  }

  const auto privateIds = PrivateAdapterIds(error);
  if (!error->empty()) {
    return {};
  }

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

  std::set<std::string> unique;
  for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }
    GUID adapterId{};
    if (!ParseAdapterGuid(adapter->AdapterName, &adapterId) || !privateIds.contains(adapterId)) {
      continue;
    }
    for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr == nullptr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      char buffer[INET_ADDRSTRLEN]{};
      const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), buffer,
                    static_cast<DWORD>(std::size(buffer))) != nullptr) {
        unique.emplace(buffer);
      }
    }
  }
  return {unique.begin(), unique.end()};
}

bool NetworkGate::IsPrivateIpv4(std::string_view address, std::string* error) {
  const auto addresses = PrivateIpv4Addresses(error);
  return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
}

}  // namespace hss::host
