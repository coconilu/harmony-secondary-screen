#pragma once

namespace hss::receiver {

enum class DecoderLifecycleState { kStopped, kStarting, kRunning, kFlushing, kStopping };
enum class FlushRecoveryAction { kResume, kRebuild };

constexpr bool DecoderCallbacksAllowed(DecoderLifecycleState state) {
  return state == DecoderLifecycleState::kRunning;
}

constexpr FlushRecoveryAction EvaluateFlushRecovery(bool flushSucceeded,
                                                     bool restartSucceeded) {
  return flushSucceeded && restartSucceeded ? FlushRecoveryAction::kResume
                                            : FlushRecoveryAction::kRebuild;
}

}  // namespace hss::receiver
