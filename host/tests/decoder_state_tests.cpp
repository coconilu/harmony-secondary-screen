#include "decoder_state.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

using hss::receiver::DecoderCallbacksAllowed;
using hss::receiver::DecoderLifecycleState;
using hss::receiver::EvaluateFlushRecovery;
using hss::receiver::FlushRecoveryAction;

int main() {
  if (!DecoderCallbacksAllowed(DecoderLifecycleState::kRunning) ||
      DecoderCallbacksAllowed(DecoderLifecycleState::kFlushing) ||
      DecoderCallbacksAllowed(DecoderLifecycleState::kStopping) ||
      EvaluateFlushRecovery(true, true) != FlushRecoveryAction::kResume ||
      EvaluateFlushRecovery(false, true) != FlushRecoveryAction::kRebuild ||
      EvaluateFlushRecovery(true, false) != FlushRecoveryAction::kRebuild) {
    std::cerr << "Decoder lifecycle policy failed\n";
    return 1;
  }

  std::atomic state{DecoderLifecycleState::kRunning};
  std::mutex callbackQueue;
  auto lifecycle = std::async(std::launch::async, [&] {
    state = DecoderLifecycleState::kStopping;
    {
      std::scoped_lock queueLock(callbackQueue);
      // Queue invalidation ends before the blocking codec lifecycle call.
    }
    // Model Stop/Destroy waiting for a callback. The callback must be able to
    // take its own queue lock, otherwise the lifecycle call would deadlock.
    auto callback = std::async(std::launch::async, [&] {
      std::scoped_lock queueLock(callbackQueue);
      return !DecoderCallbacksAllowed(state.load());
    });
    if (callback.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
      return false;
    }
    return callback.get();
  });
  if (lifecycle.wait_for(std::chrono::seconds(1)) != std::future_status::ready ||
      !lifecycle.get()) {
    std::cerr << "Decoder callback/lifecycle separation deadlocked\n";
    return 1;
  }
  std::cout << "Decoder lifecycle and callback-lock model passed\n";
  return 0;
}
