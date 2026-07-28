#pragma once

#include "decoder_state.h"

#include <atomic>
#include <cstddef>
#include <utility>

namespace hss::receiver {

class DecoderCallbackGate final {
 public:
  DecoderLifecycleState state() const {
    return state_.load();
  }

  void SetState(DecoderLifecycleState state) {
    state_.store(state);
  }

  bool CallbacksAllowed() const {
    return DecoderCallbacksAllowed(state());
  }

  template <typename ClearQueues>
  void BeginFlush(ClearQueues&& clearQueues) {
    // Closing the callback gate must happen before queue clearing. A NeedInput
    // callback that raced with this transition either observes kFlushing or is
    // admitted before the transition and is then removed by clearQueues.
    SetState(DecoderLifecycleState::kFlushing);
    std::forward<ClearQueues>(clearQueues)();
  }

 private:
  std::atomic<DecoderLifecycleState> state_{DecoderLifecycleState::kStopped};
};

class DecoderRecoveryCoordinator final {
 public:
  void Reset() {
    state_.store(DecoderRecoveryState::kNeedsCodecData);
    keyframe_request_pending_.store(false);
  }

  DecoderRecoveryState state() const {
    return state_.load();
  }

  bool InputAllowed(DecoderInputKind kind) const {
    return DecoderInputAllowed(state(), kind);
  }

  void RequireCodecData() {
    state_.store(DecoderRecoveryState::kNeedsCodecData);
  }

  void RequestKeyFrame() {
    keyframe_request_pending_.store(true);
  }

  bool ConsumeKeyFrameRequest() {
    return keyframe_request_pending_.exchange(false);
  }

  template <typename ClearPending>
  DecodeQueueAdmission Admit(std::size_t queuedFrames, std::size_t capacity,
                             ClearPending&& clearPending) {
    const auto admission =
        EvaluateDecodeQueueAdmission(queuedFrames, capacity, state());
    if (!admission.accepted) {
      if (admission.clearPending) {
        std::forward<ClearPending>(clearPending)();
      }
      state_.store(admission.nextState);
      if (admission.requestKeyFrame) {
        RequestKeyFrame();
      }
    }
    return admission;
  }

  void OnInputSubmitted(DecoderInputKind kind, bool submitted) {
    state_.store(AdvanceDecoderRecovery(state(), kind, submitted));
    if (!submitted) {
      RequestKeyFrame();
    }
  }

 private:
  std::atomic<DecoderRecoveryState> state_{
      DecoderRecoveryState::kNeedsCodecData};
  std::atomic<bool> keyframe_request_pending_{false};
};

}  // namespace hss::receiver
