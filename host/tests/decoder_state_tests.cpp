#include "avc_decoder_input.h"
#include "bounded_control_queue.h"
#include "decoder_state.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

using hss::receiver::DecoderCallbacksAllowed;
using hss::receiver::DecoderInputAllowed;
using hss::receiver::DecoderInputKind;
using hss::receiver::DecoderLifecycleState;
using hss::receiver::DecoderRecoveryState;
using hss::receiver::EvaluateFlushRecovery;
using hss::receiver::FlushRecoveryAction;

int main() {
  if (!DecoderCallbacksAllowed(DecoderLifecycleState::kRunning) ||
      DecoderCallbacksAllowed(DecoderLifecycleState::kFlushing) ||
      DecoderCallbacksAllowed(DecoderLifecycleState::kStopping) ||
      EvaluateFlushRecovery(true, true) != FlushRecoveryAction::kResume ||
      EvaluateFlushRecovery(false, true) != FlushRecoveryAction::kRebuild ||
      EvaluateFlushRecovery(true, false) != FlushRecoveryAction::kRebuild ||
      !DecoderInputAllowed(DecoderRecoveryState::kNeedsCodecData,
                           DecoderInputKind::kCodecData) ||
      DecoderInputAllowed(DecoderRecoveryState::kNeedsCodecData,
                          DecoderInputKind::kSyncFrame) ||
      hss::receiver::AdvanceDecoderRecovery(DecoderRecoveryState::kNeedsCodecData,
                                            DecoderInputKind::kCodecData, true) !=
          DecoderRecoveryState::kNeedsSyncFrame ||
      hss::receiver::AdvanceDecoderRecovery(DecoderRecoveryState::kNeedsCodecData,
                                            DecoderInputKind::kCodecData, false) !=
          DecoderRecoveryState::kNeedsCodecData ||
      hss::receiver::AdvanceDecoderRecovery(DecoderRecoveryState::kNeedsSyncFrame,
                                            DecoderInputKind::kSyncFrame, true) !=
          DecoderRecoveryState::kReady) {
    std::cerr << "Decoder lifecycle policy failed\n";
    return 1;
  }

  const std::vector<std::byte> recoveryAccessUnit{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x67},
      std::byte{0x42}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
      std::byte{0x68}, std::byte{0xce}, std::byte{0}, std::byte{0}, std::byte{1},
      std::byte{0x65}, std::byte{0x88}};
  const auto recovery = hss::receiver::SplitAvcRecoveryInput(recoveryAccessUnit);
  if (!recovery.complete() || recovery.codecData.empty() || recovery.syncFrame.empty()) {
    std::cerr << "SPS/PPS and IDR were not separated into decoder inputs\n";
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

  hss::receiver::BoundedControlQueue telemetry(8);
  auto hostNotReading = std::async(std::launch::async, [&] {
    for (int index = 0; index < 10'000; ++index) {
      telemetry.Push("telemetry");
    }
    state = DecoderLifecycleState::kStopping;
    return true;
  });
  if (hostNotReading.wait_for(std::chrono::seconds(1)) != std::future_status::ready ||
      !hostNotReading.get() || telemetry.size() != 8) {
    std::cerr << "Unread telemetry blocked the decoder lifecycle model\n";
    return 1;
  }
  std::cout << "Decoder lifecycle and callback-lock model passed\n";
  return 0;
}
