#pragma once

#include <cstddef>

namespace hss::graphics {

class AsyncMftSchedule final {
 public:
  void OnNeedInput() { ++input_credits_; }
  bool TryConsumeInputCredit(bool inputAvailable) {
    if (!inputAvailable || input_credits_ == 0) return false;
    --input_credits_;
    return true;
  }
  std::size_t input_credits() const { return input_credits_; }
  void OnHaveOutput() { ++output_credits_; }
  bool TryConsumeOutputCredit() {
    if (output_credits_ == 0) return false;
    --output_credits_;
    return true;
  }
  std::size_t output_credits() const { return output_credits_; }

 private:
  std::size_t input_credits_ = 0;
  std::size_t output_credits_ = 0;
};

}  // namespace hss::graphics
