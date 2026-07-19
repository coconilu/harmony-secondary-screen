#pragma once

#include <string>
#include <string_view>

namespace hss::host {

class PointerInjector final {
 public:
  struct DisplayRect {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
  };

  bool HandleControlMessage(std::string_view json, std::string* error) const;
  static bool IsHarmonyAdapterPath(std::wstring_view path);

 private:
  static bool FindHarmonyDisplay(DisplayRect* result);
};

}  // namespace hss::host
