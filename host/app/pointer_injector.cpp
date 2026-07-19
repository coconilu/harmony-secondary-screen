#include "pointer_injector.h"

#include "hss_protocol.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace hss::host {
namespace {

struct EnumContext {
  PointerInjector::DisplayRect* rect;
  bool found;
};

BOOL CALLBACK EnumMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto* context = reinterpret_cast<EnumContext*>(data);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) {
    return TRUE;
  }
  DISPLAY_DEVICEW device{};
  device.cb = sizeof(device);
  if (!EnumDisplayDevicesW(info.szDevice, 0, &device, 0)) {
    return TRUE;
  }
  if (std::wcsstr(device.DeviceString, L"Harmony Secondary Screen") == nullptr) {
    return TRUE;
  }
  context->rect->left = info.rcMonitor.left;
  context->rect->top = info.rcMonitor.top;
  context->rect->right = info.rcMonitor.right;
  context->rect->bottom = info.rcMonitor.bottom;
  context->found = true;
  return FALSE;
}

}  // namespace

bool PointerInjector::FindHarmonyDisplay(DisplayRect* result) {
  if (result == nullptr) {
    return false;
  }
  EnumContext context{result, false};
  EnumDisplayMonitors(nullptr, nullptr, EnumMonitor, reinterpret_cast<LPARAM>(&context));
  return context.found;
}

bool PointerInjector::HandleControlMessage(std::string_view json, std::string* error) const {
  if (error == nullptr) {
    return false;
  }
  const auto type = protocol::JsonString(json, "type");
  if (type != "pointer") {
    return true;
  }
  const auto action = protocol::JsonString(json, "action");
  const auto x = protocol::JsonNumber(json, "x");
  const auto y = protocol::JsonNumber(json, "y");
  if (!action || !x || !y || *x < 0.0 || *x > 1.0 || *y < 0.0 || *y > 1.0) {
    *error = "invalid pointer message";
    return false;
  }

  DisplayRect display{};
  if (!FindHarmonyDisplay(&display)) {
    *error = "Harmony virtual display is not active";
    return false;
  }

  const long virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const long virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const long virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const long virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (virtualWidth <= 1 || virtualHeight <= 1) {
    *error = "invalid Windows virtual desktop bounds";
    return false;
  }

  const double screenX = static_cast<double>(display.left) +
                         (*x * static_cast<double>(display.right - display.left - 1));
  const double screenY = static_cast<double>(display.top) +
                         (*y * static_cast<double>(display.bottom - display.top - 1));
  const auto absoluteX = static_cast<LONG>(std::llround(
      ((screenX - virtualLeft) * 65535.0) / static_cast<double>(virtualWidth - 1)));
  const auto absoluteY = static_cast<LONG>(std::llround(
      ((screenY - virtualTop) * 65535.0) / static_cast<double>(virtualHeight - 1)));

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = std::clamp<LONG>(absoluteX, 0, 65535);
  input.mi.dy = std::clamp<LONG>(absoluteY, 0, 65535);
  input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
  if (*action == "down") {
    input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
  } else if (*action == "up") {
    input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
  } else if (*action == "move") {
    // Movement flags already set.
  } else if (*action == "scroll") {
    const auto deltaY = protocol::JsonNumber(json, "deltaY");
    if (!deltaY || std::abs(*deltaY) > 1.0) {
      *error = "invalid scroll delta";
      return false;
    }
    input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(static_cast<LONG>(std::llround(-*deltaY * WHEEL_DELTA * 3.0)));
  } else {
    *error = "unsupported pointer action";
    return false;
  }

  if (SendInput(1, &input, sizeof(input)) != 1) {
    *error = "SendInput failed: " + std::to_string(GetLastError());
    return false;
  }
  return true;
}

}  // namespace hss::host
