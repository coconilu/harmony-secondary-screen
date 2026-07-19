#include "hss_protocol.h"
#include "pointer_injector.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI ConsoleHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    g_running = false;
    return TRUE;
  }
  return FALSE;
}

}  // namespace

int wmain() {
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
  hss::host::PointerInjector injector;
  std::cout << "Harmony Secondary Screen 输入代理已在当前用户会话启动。\n";
  while (g_running) {
    HANDLE pipe = CreateNamedPipeW(
        L"\\\\.\\pipe\\HarmonySecondaryScreen.Input", PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64U * 1024U, 64U * 1024U, 1000, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    bool connected = false;
    while (g_running) {
      if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
        connected = true;
        break;
      }
      const DWORD error = GetLastError();
      if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA) break;
      Sleep(50);
    }
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }

    hss::protocol::ControlFrameDecoder decoder;
    std::array<std::byte, 8192> buffer{};
    while (g_running) {
      DWORD available = 0;
      if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
      if (available == 0) {
        Sleep(2);
        continue;
      }
      DWORD read = 0;
      if (!ReadFile(pipe, buffer.data(),
                    static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available)),
                    &read, nullptr) || read == 0) {
        break;
      }
      std::vector<std::string> frames;
      std::string parseError;
      if (!decoder.Push(std::span(buffer).first(read), &frames, &parseError)) break;
      for (const auto& json : frames) {
        std::string inputError;
        if (!injector.HandleControlMessage(json, &inputError)) {
          std::cerr << inputError << '\n';
        }
      }
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
  return 0;
}
