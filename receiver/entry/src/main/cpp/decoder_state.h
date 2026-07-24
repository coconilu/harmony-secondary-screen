#pragma once

namespace hss::receiver {

enum class DecoderLifecycleState { kStopped, kStarting, kRunning, kFlushing, kStopping };
enum class FlushRecoveryAction { kResume, kRebuild };
enum class DecoderInputKind { kCodecData, kSyncFrame, kFrame };
enum class DecoderRecoveryState { kNeedsCodecData, kNeedsSyncFrame, kReady };

constexpr bool DecoderCallbacksAllowed(DecoderLifecycleState state) {
  return state == DecoderLifecycleState::kRunning;
}

constexpr FlushRecoveryAction EvaluateFlushRecovery(bool flushSucceeded,
                                                     bool restartSucceeded) {
  return flushSucceeded && restartSucceeded ? FlushRecoveryAction::kResume
                                            : FlushRecoveryAction::kRebuild;
}

constexpr bool DecoderInputAllowed(DecoderRecoveryState state, DecoderInputKind kind) {
  if (state == DecoderRecoveryState::kNeedsCodecData) {
    return kind == DecoderInputKind::kCodecData;
  }
  if (state == DecoderRecoveryState::kNeedsSyncFrame) {
    return kind == DecoderInputKind::kSyncFrame;
  }
  return kind == DecoderInputKind::kSyncFrame || kind == DecoderInputKind::kFrame;
}

constexpr DecoderRecoveryState AdvanceDecoderRecovery(DecoderRecoveryState state,
                                                       DecoderInputKind kind,
                                                       bool pushed) {
  if (!pushed) return DecoderRecoveryState::kNeedsCodecData;
  if (state == DecoderRecoveryState::kNeedsCodecData &&
      kind == DecoderInputKind::kCodecData) {
    return DecoderRecoveryState::kNeedsSyncFrame;
  }
  if (state == DecoderRecoveryState::kNeedsSyncFrame &&
      kind == DecoderInputKind::kSyncFrame) {
    return DecoderRecoveryState::kReady;
  }
  return state;
}

}  // namespace hss::receiver
