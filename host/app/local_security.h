#pragma once

#include <windows.h>

namespace hss::host {

// UMDF 2 runs in LocalService. The frame pipe grants that principal write-only
// access; the SCM service (LocalSystem) and administrators retain full access.
inline constexpr wchar_t kFramesPipeSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GW;;;LS)(A;;GA;;;BA)";
inline constexpr wchar_t kKeyframeEventSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GR;;;LS)(A;;GA;;;BA)";

class LocalSecurityAttributes final {
 public:
  explicit LocalSecurityAttributes(const wchar_t* sddl);
  ~LocalSecurityAttributes();
  LocalSecurityAttributes(const LocalSecurityAttributes&) = delete;
  LocalSecurityAttributes& operator=(const LocalSecurityAttributes&) = delete;

  SECURITY_ATTRIBUTES* get() { return valid() ? &attributes_ : nullptr; }
  bool valid() const { return descriptor_ != nullptr; }

 private:
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  SECURITY_ATTRIBUTES attributes_{};
};

}  // namespace hss::host
