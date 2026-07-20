#include "host_server.h"
#include "hss_protocol.h"
#include "network_gate.h"

#include <windows.h>

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kServiceName[] = L"HarmonySecondaryScreenHost";
constexpr wchar_t kStatusPipe[] = L"\\\\.\\pipe\\HarmonySecondaryScreen.Status";

hss::host::HostServer* g_server = nullptr;
SERVICE_STATUS_HANDLE g_service_handle = nullptr;
SERVICE_STATUS g_service_status{};
DWORD g_checkpoint = 1;
std::vector<std::wstring> g_allowed_wifi_profile_ids;

std::wstring ConfirmWifiAccess(std::string* error) {
  auto profiles = hss::host::NetworkGate::ConnectedWifiProfiles(error);
  if (!error->empty()) {
    return {};
  }

  for (const auto& profile : profiles) {
    const std::wstring message =
        L"是否允许 Harmony Secondary Screen 在 Wi-Fi“" + profile.name +
        L"”上等待平板连接？\n\n应用只会监听该物理 Wi-Fi 的当前 IPv4 地址，"
        L"不会修改 Windows 的公用/专用网络类型，也不会修改或断开 VPN。\n\n"
        L"机场、酒店、公司访客网络请点击“否”。";
    const int choice = MessageBoxW(nullptr, message.c_str(), L"Harmony Secondary Screen",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
    if (choice == IDYES) {
      return profile.id;
    }
  }
  *error = profiles.empty() ? "未找到已连接并取得 IPv4 地址的物理 Wi-Fi"
                            : "用户未允许应用访问当前 Wi-Fi";
  return {};
}

void ReportServiceStatus(DWORD state, DWORD error = NO_ERROR) {
  if (g_service_handle == nullptr) return;
  g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_service_status.dwCurrentState = state;
  g_service_status.dwWin32ExitCode = error;
  g_service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
  g_service_status.dwWaitHint =
      (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 5000U : 0U;
  g_service_status.dwCheckPoint =
      (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? g_checkpoint++ : 0U;
  SetServiceStatus(g_service_handle, &g_service_status);
}

void WINAPI ServiceControlHandler(DWORD control) {
  if (control == SERVICE_CONTROL_STOP && g_server != nullptr) {
    ReportServiceStatus(SERVICE_STOP_PENDING);
    g_server->Stop();
  }
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
  g_service_handle = RegisterServiceCtrlHandlerW(kServiceName, ServiceControlHandler);
  if (g_service_handle == nullptr) return;
  ReportServiceStatus(SERVICE_START_PENDING);

  hss::host::HostServer server(g_allowed_wifi_profile_ids);
  g_server = &server;
  std::string error;
  if (!server.Start(&error)) {
    g_server = nullptr;
    ReportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    return;
  }
  ReportServiceStatus(SERVICE_RUNNING);
  server.Wait();
  g_server = nullptr;
  ReportServiceStatus(SERVICE_STOPPED);
}

BOOL WINAPI ConsoleHandler(DWORD signal) {
  if ((signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) &&
      g_server != nullptr) {
    g_server->Stop();
    return TRUE;
  }
  return FALSE;
}

int RunConsole() {
  std::string networkError;
  const auto allowedProfileId = ConfirmWifiAccess(&networkError);
  if (allowedProfileId.empty()) {
    std::cerr << "Host 启动失败: " << networkError << '\n';
    return 3;
  }
  hss::host::HostServer server({allowedProfileId});
  g_server = &server;
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);

  std::string error;
  if (!server.Start(&error)) {
    std::cerr << "Host 启动失败: " << error << '\n';
    g_server = nullptr;
    return 1;
  }
  std::cout << "Harmony Secondary Screen Host 已启动\n"
            << "一次性配对码: " << server.pairing_code() << '\n'
            << "按 Ctrl+C 停止。\n";
  server.Wait();
  g_server = nullptr;
  return 0;
}

int ShowPairingCode() {
  if (!WaitNamedPipeW(kStatusPipe, 2000)) {
    std::cerr << "Host 服务未运行或状态管道不可用。\n";
    return 4;
  }
  HANDLE pipe = CreateFileW(kStatusPipe, GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    std::cerr << "无法连接 Host 状态管道。\n";
    return 4;
  }
  std::array<std::byte, 4096> buffer{};
  DWORD read = 0;
  const BOOL ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
  CloseHandle(pipe);
  if (!ok || read == 0) {
    std::cerr << "无法读取 Host 状态。\n";
    return 4;
  }
  hss::protocol::ControlFrameDecoder decoder;
  std::vector<std::string> frames;
  std::string error;
  if (!decoder.Push(std::span(buffer).first(read), &frames, &error) || frames.size() != 1) {
    std::cerr << "Host 状态格式无效。\n";
    return 4;
  }
  const auto code = hss::protocol::JsonString(frames.front(), "pairingCode");
  if (!code) {
    std::cerr << "Host 未返回配对码。\n";
    return 4;
  }
  std::cout << *code << '\n';
  return 0;
}

}  // namespace

int wmain(int argumentCount, wchar_t** arguments) {
  const std::wstring_view option = argumentCount >= 2 ? arguments[1] : L"";
  if (option == L"--list-wifi" && argumentCount == 2) {
    std::string error;
    const auto profiles = hss::host::NetworkGate::ConnectedWifiProfiles(&error);
    if (!error.empty()) {
      std::cerr << error << '\n';
      return 3;
    }
    for (const auto& profile : profiles) {
      std::wcout << profile.id << L'\t' << profile.name;
      for (const auto& address : profile.ipv4_addresses) {
        std::wcout << L'\t' << std::wstring(address.begin(), address.end());
      }
      std::wcout << L'\n';
    }
    return profiles.empty() ? 3 : 0;
  }
  if (option == L"--check-network" && argumentCount == 3) {
    std::string error;
    const auto addresses =
        hss::host::NetworkGate::AllowedWifiIpv4Addresses({arguments[2]}, &error);
    if (addresses.empty()) {
      std::cerr << (error.empty() ? "The approved physical Wi-Fi is not connected." : error)
                << '\n';
      return 3;
    }
    for (const auto& address : addresses) std::cout << address << '\n';
    return 0;
  }
  if (option == L"--prepare-network" && argumentCount == 2) {
    std::string error;
    const auto profileId = ConfirmWifiAccess(&error);
    if (profileId.empty()) {
      std::cerr << "Wi-Fi 授权失败: " << error << '\n';
      return 3;
    }
    std::wcout << profileId << '\n';
    return 0;
  }
  if (option == L"--pairing-code") return ShowPairingCode();
  if (option == L"--service" && argumentCount == 3) {
    g_allowed_wifi_profile_ids = {arguments[2]};
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<wchar_t*>(kServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(serviceTable)) return 2;
    return 0;
  }
  if (argumentCount == 1 || option == L"--console") return RunConsole();
  std::cerr << "Usage: hss_host.exe [--console|--prepare-network|--list-wifi|"
               "--check-network <profile-id>|--service <profile-id>|--pairing-code]\n";
  return 2;
}
