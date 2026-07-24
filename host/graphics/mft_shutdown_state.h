#pragma once

namespace hss::graphics {

class MftShutdownState final {
 public:
  bool Begin() {
    if (shutdown_) return false;
    shutdown_ = true;
    return true;
  }
  void Reset() { shutdown_ = false; }

 private:
  bool shutdown_ = false;
};

}  // namespace hss::graphics
