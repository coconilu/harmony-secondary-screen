#include "pointer_injector.h"

#include "hss_protocol.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace hss::host {
namespace {

constexpr std::wstring_view kAdapterHardwareId = L"root#harmonysecondaryscreenidd";

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), [](wchar_t lhs, wchar_t rhs) {
           return std::towlower(lhs) == std::towlower(rhs);
         });
}

bool MonitorRectForGdiName(std::wstring_view gdiName, PointerInjector::DisplayRect* rect) {
  struct Context {
    std::wstring_view gdiName;
    PointerInjector::DisplayRect* rect;
    bool found = false;
  } context{gdiName, rect};
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto* context = reinterpret_cast<Context*>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info) ||
            !EqualsIgnoreCase(info.szDevice, context->gdiName)) {
          return TRUE;
        }
        context->rect->left = info.rcMonitor.left;
        context->rect->top = info.rcMonitor.top;
        context->rect->right = info.rcMonitor.right;
        context->rect->bottom = info.rcMonitor.bottom;
        context->found = true;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&context));
  return context.found;
}

}  // namespace

bool PointerInjector::IsHarmonyAdapterPath(std::wstring_view path) {
  std::wstring normalized(path);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](wchar_t value) { return std::towlower(value); });
  return normalized.find(kAdapterHardwareId) != std::wstring::npos;
}

bool PointerInjector::FindHarmonyDisplay(DisplayRect* result) {
  if (result == nullptr) return false;

  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
  if (status != ERROR_SUCCESS) return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                              modes.data(), nullptr);
  if (status != ERROR_SUCCESS) return false;
  paths.resize(pathCount);

  for (const auto& path : paths) {
    DISPLAYCONFIG_ADAPTER_NAME adapter{};
    adapter.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    adapter.header.size = sizeof(adapter);
    adapter.header.adapterId = path.targetInfo.adapterId;
    if (DisplayConfigGetDeviceInfo(&adapter.header) != ERROR_SUCCESS ||
        !IsHarmonyAdapterPath(adapter.adapterDevicePath) || path.targetInfo.id != 0) {
      continue;
    }

    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
        MonitorRectForGdiName(source.viewGdiDeviceName, result)) {
      return true;
    }
  }
  return false;
}

bool PointerInjector::HandleControlMessage(std::string_view json, std::string* error) const {
  if (error == nullptr) return false;
  const auto type = protocol::JsonString(json, "type");
  if (type != "pointer") return true;
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
    input.mi.mouseData =
        static_cast<DWORD>(static_cast<LONG>(std::llround(-*deltaY * WHEEL_DELTA * 3.0)));
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
