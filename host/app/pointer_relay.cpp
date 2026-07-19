#include "pointer_relay.h"

#include "hss_protocol.h"

namespace hss::host {

PointerRelay::~PointerRelay() {
  std::scoped_lock lock(mutex_);
  Close();
}

bool PointerRelay::Connect(std::string* error) {
  if (pipe_ != INVALID_HANDLE_VALUE) return true;
  pipe_ = CreateFileW(L"\\\\.\\pipe\\HarmonySecondaryScreen.Input", GENERIC_WRITE, 0,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    *error = "用户会话输入代理未运行";
    return false;
  }
  return true;
}

void PointerRelay::Close() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
}

bool PointerRelay::Send(std::string_view json, std::string* error) {
  if (error == nullptr) return false;
  std::scoped_lock lock(mutex_);
  if (!Connect(error)) return false;
  const auto frame = protocol::EncodeControlFrame(json);
  std::size_t total = 0;
  while (total < frame.size()) {
    DWORD written = 0;
    if (!WriteFile(pipe_, frame.data() + total,
                   static_cast<DWORD>(frame.size() - total), &written, nullptr) || written == 0) {
      Close();
      *error = "输入代理命名管道中断";
      return false;
    }
    total += written;
  }
  return true;
}

}  // namespace hss::host
