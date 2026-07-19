#include "host_server.h"
#include "hss_protocol.h"
#include "network_gate.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
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

bool LaunchElevatedWifiTrust(std::wstring_view networkId, std::string* error) {
  std::array<wchar_t, 32768> executable{};
  const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                         static_cast<DWORD>(executable.size()));
  if (length == 0 || length >= static_cast<DWORD>(executable.size())) {
    *error = "无法确定 Host 可执行文件路径";
    return false;
  }

  const std::wstring parameters = L"--trust-wifi \"" + std::wstring(networkId) + L"\"";
  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  execute.lpVerb = L"runas";
  execute.lpFile = executable.data();
  execute.lpParameters = parameters.c_str();
  execute.nShow = SW_HIDE;
  if (!ShellExecuteExW(&execute)) {
    const DWORD shellError = GetLastError();
    *error = shellError == ERROR_CANCELLED ? "用户取消了管理员授权"
                                           : "启动网络配置助手失败: " + std::to_string(shellError);
    return false;
  }
  if (execute.hProcess == nullptr) {
    *error = "网络配置助手未返回进程句柄";
    return false;
  }
  WaitForSingleObject(execute.hProcess, INFINITE);
  DWORD exitCode = ERROR_GEN_FAILURE;
  const BOOL readExitCode = GetExitCodeProcess(execute.hProcess, &exitCode);
  CloseHandle(execute.hProcess);
  if (!readExitCode || exitCode != 0) {
    *error = "网络配置助手未能信任当前 Wi-Fi";
    return false;
  }
  return true;
}

bool PrepareTrustedWifi(std::string* error) {
  auto profiles = hss::host::NetworkGate::ConnectedWifiProfiles(error);
  if (!error->empty()) {
    return false;
  }
  if (std::ranges::any_of(profiles, [](const hss::host::WifiNetworkProfile& profile) {
        return profile.is_private && !profile.ipv4_addresses.empty();
      })) {
    return true;
  }

  bool foundPublicWifi = false;
  bool foundMixedPublicWifi = false;
  for (const auto& profile : profiles) {
    if (profile.is_private || profile.ipv4_addresses.empty()) {
      continue;
    }
    if (!profile.is_wifi_only) {
      foundMixedPublicWifi = true;
      continue;
    }
    foundPublicWifi = true;
    const std::wstring message =
        L"Windows 将 Wi-Fi“" + profile.name +
        L"”标记为公用网络。\n\n仅当这是你信任的家庭或私人 Wi-Fi 时，点击“是”。"
        L"随后 Windows 会请求管理员授权，并只把这个 Wi-Fi 标记为专用网络。\n\n"
        L"机场、酒店、公司访客网络请点击“否”。";
    const int choice = MessageBoxW(nullptr, message.c_str(), L"Harmony Secondary Screen",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
    if (choice != IDYES) {
      continue;
    }
    if (!LaunchElevatedWifiTrust(profile.id, error)) {
      return false;
    }
    const auto trustedAddresses = hss::host::NetworkGate::TrustedWifiIpv4Addresses(error);
    if (!error->empty()) {
      return false;
    }
    if (!trustedAddresses.empty()) {
      return true;
    }
    *error = "当前 Wi-Fi 已授权，但 Windows 尚未把它识别为专用网络";
    return false;
  }
  if (foundPublicWifi) {
    *error = "用户未信任当前公用 Wi-Fi";
  } else if (foundMixedPublicWifi) {
    *error = "当前公用 Wi-Fi 与 VPN、以太网或未知连接被 Windows 聚合，无法安全自动修改；"
             "请暂时断开合并的连接，完成一次信任后可重新连接 VPN";
  } else {
    *error = "未找到已连接并取得 IPv4 地址的物理 Wi-Fi";
  }
  return false;
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

  hss::host::HostServer server;
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
  if (!PrepareTrustedWifi(&networkError)) {
    std::cerr << "Host 启动失败: " << networkError << '\n';
    return 3;
  }
  hss::host::HostServer server;
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
  const std::wstring_view option = argumentCount == 2 ? arguments[1] : L"";
  if (option == L"--check-network") {
    std::string error;
    const auto addresses = hss::host::NetworkGate::TrustedWifiIpv4Addresses(&error);
    if (addresses.empty()) {
      std::cerr << (error.empty() ? "No trusted physical Wi-Fi IPv4 address." : error) << '\n';
      return 3;
    }
    for (const auto& address : addresses) std::cout << address << '\n';
    return 0;
  }
  if (option == L"--prepare-network") {
    std::string error;
    if (!PrepareTrustedWifi(&error)) {
      std::cerr << "Wi-Fi 准备失败: " << error << '\n';
      return 3;
    }
    return 0;
  }
  if (argumentCount == 3 && std::wstring_view(arguments[1]) == L"--trust-wifi") {
    std::string error;
    if (!hss::host::NetworkGate::TrustWifiProfile(arguments[2], &error)) {
      std::cerr << "信任 Wi-Fi 失败: " << error << '\n';
      return 5;
    }
    return 0;
  }
  if (option == L"--pairing-code") return ShowPairingCode();
  if (option == L"--service") {
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<wchar_t*>(kServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(serviceTable)) return 2;
    return 0;
  }
  if (argumentCount == 1 || option == L"--console") return RunConsole();
  std::cerr << "Usage: hss_host.exe [--service|--console|--prepare-network|--check-network|"
               "--pairing-code]\n";
  return 2;
}
