#include "async_mft_schedule.h"

#include <iostream>

int main() {
  hss::graphics::AsyncMftSchedule schedule;
  schedule.OnNeedInput();
  schedule.OnNeedInput();
  schedule.OnNeedInput();
  if (!schedule.TryConsumeInputCredit(true) || !schedule.TryConsumeInputCredit(true) ||
      schedule.input_credits() != 1 || schedule.TryConsumeInputCredit(false) ||
      !schedule.TryConsumeInputCredit(true) || schedule.TryConsumeInputCredit(true)) {
    std::cerr << "Async MFT pre-warmed multi-input schedule failed\n";
    return 1;
  }
  schedule.OnHaveOutput();
  schedule.OnHaveOutput();
  if (!schedule.TryConsumeOutputCredit() || !schedule.TryConsumeOutputCredit() ||
      schedule.TryConsumeOutputCredit()) {
    std::cerr << "Async MFT multi-output schedule failed\n";
    return 1;
  }
  std::cout << "Async MFT independent input/output schedule passed\n";
  return 0;
}
