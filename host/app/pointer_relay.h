#pragma once

#include <windows.h>

#include <mutex>
#include <string>
#include <string_view>

namespace hss::host {

// Relays authenticated pointer JSON from the Session-0 Host service to the
// per-user input agent. This keeps SendInput out of the service session.
class PointerRelay final {
 public:
  ~PointerRelay();
  bool Send(std::string_view json, std::string* error);

 private:
  bool Connect(std::string* error);
  void Close();

  std::mutex mutex_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}  // namespace hss::host
