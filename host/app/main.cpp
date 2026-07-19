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
    const auto addresses = hss::host::NetworkGate::PrivateIpv4Addresses(&error);
    if (addresses.empty()) {
      std::cerr << (error.empty() ? "No private-network IPv4 address." : error) << '\n';
      return 3;
    }
    for (const auto& address : addresses) std::cout << address << '\n';
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
  std::cerr << "Usage: hss_host.exe [--service|--console|--check-network|--pairing-code]\n";
  return 2;
}
