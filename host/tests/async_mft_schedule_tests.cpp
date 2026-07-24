#include "async_mft_schedule.h"
#include "mft_shutdown_state.h"

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
  struct FakeMft {
    int shutdownCount = 0;
    void Shutdown() { ++shutdownCount; }
  } fake;
  hss::graphics::MftShutdownState shutdownState;
  if (shutdownState.Begin()) fake.Shutdown();
  if (shutdownState.Begin()) fake.Shutdown();
  if (fake.shutdownCount != 1) {
    std::cerr << "Async MFT was not shut down exactly once\n";
    return 1;
  }
  shutdownState.Reset();
  if (shutdownState.Begin()) fake.Shutdown();
  if (fake.shutdownCount != 2) {
    std::cerr << "Replacement MFT did not receive its own shutdown\n";
    return 1;
  }
  std::cout << "Async MFT independent input/output schedule passed\n";
  return 0;
}
