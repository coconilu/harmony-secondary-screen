#include "../entry/src/main/cpp/avc_decoder_input.h"
#include "../entry/src/main/cpp/decoder_orchestration.h"
#include "../entry/src/main/cpp/decoder_state.h"
#include "../entry/src/main/cpp/receiver_lifecycle_state.h"
#include "../entry/src/main/cpp/video_surface_layout.h"

#include <cstddef>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <vector>

namespace {

using hss::receiver::DecoderLifecycleState;
using hss::receiver::DecoderInputKind;
using hss::receiver::DecoderCallbackGate;
using hss::receiver::DecoderRecoveryCoordinator;
using hss::receiver::DecoderRecoveryState;
using hss::receiver::DecoderRuntimeSnapshot;
using hss::receiver::ReceiverLifecycleDecision;
using hss::receiver::ReceiverLifecycleState;

constexpr std::uintptr_t kComponent1 = 0x10;
constexpr std::uintptr_t kComponent2 = 0x20;
constexpr std::uintptr_t kSurface1 = 0x1000;
constexpr std::uintptr_t kSurface2 = 0x2000;
constexpr std::uintptr_t kSurface3 = 0x3000;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::vector<std::byte> CompleteRecoveryAccessUnit() {
  return {
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
      std::byte{0x67}, std::byte{0x42}, std::byte{0x00}, std::byte{0x1f},
      std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x68},
      std::byte{0xce}, std::byte{0x06}, std::byte{0xe2},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
      std::byte{0x65}, std::byte{0x88}, std::byte{0x84}};
}

struct LifecycleHarness final {
  ReceiverLifecycleState lifecycle;
  DecoderRuntimeSnapshot decoder;
  int starts = 0;
  int stops = 0;

  ReceiverLifecycleDecision Apply(ReceiverLifecycleDecision decision) {
    if (decision.stopDecoder) {
      ++stops;
      decoder = {DecoderLifecycleState::kStopped, false};
    }
    if (decision.startDecoder) {
      ++starts;
      decoder = {DecoderLifecycleState::kRunning, true};
    }
    return decision;
  }

  ReceiverLifecycleDecision Created(std::uintptr_t component,
                                    std::uintptr_t surface) {
    return Apply(lifecycle.SurfaceCreated(component, surface, decoder));
  }

  ReceiverLifecycleDecision Changed(std::uintptr_t component,
                                    std::uintptr_t surface) {
    return Apply(lifecycle.SurfaceChanged(component, surface, decoder));
  }

  ReceiverLifecycleDecision Destroyed(std::uintptr_t component,
                                      std::uintptr_t surface) {
    return Apply(lifecycle.SurfaceDestroyed(component, surface, decoder));
  }

  ReceiverLifecycleDecision Foreground() {
    return Apply(lifecycle.Foreground(decoder));
  }

  ReceiverLifecycleDecision Background() {
    return Apply(lifecycle.Background(decoder));
  }
};

void TestNewSurfaceCreatedBeforeOldDestroyed() {
  LifecycleHarness harness;
  harness.Created(kComponent1, kSurface1);
  Expect(harness.starts == 1 && harness.stops == 0,
         "first Surface must start the decoder");

  const auto replacement = harness.Created(kComponent2, kSurface2);
  Expect(replacement.accepted && replacement.stopDecoder &&
             replacement.startDecoder,
         "new Surface must atomically replace the old decoder target");
  Expect(harness.lifecycle.component() == kComponent2 &&
             harness.lifecycle.surface() == kSurface2,
         "new Surface identity must become current");

  const auto staleDestroy = harness.Destroyed(kComponent1, kSurface1);
  Expect(!staleDestroy.accepted && harness.starts == 2 && harness.stops == 1,
         "old Surface destruction after replacement must be ignored");
  Expect(harness.decoder.available &&
             harness.lifecycle.surface() == kSurface2,
         "stale destruction must leave the replacement decoder running");
}

void TestOldSurfaceDestroyedBeforeNewCreated() {
  LifecycleHarness harness;
  harness.Created(kComponent1, kSurface1);
  const auto destroy = harness.Destroyed(kComponent1, kSurface1);
  Expect(destroy.accepted && destroy.stopDecoder && !destroy.startDecoder,
         "current Surface destruction must stop the decoder");
  Expect(harness.lifecycle.surface() == 0 && !harness.decoder.available,
         "destroyed Surface must be cleared");

  const auto replacement = harness.Created(kComponent2, kSurface2);
  Expect(replacement.accepted && !replacement.stopDecoder &&
             replacement.startDecoder,
         "new Surface after destruction must start a fresh decoder");
}

void TestDuplicateCallbacksAreIdempotent() {
  LifecycleHarness harness;
  harness.Created(kComponent1, kSurface1);

  const auto duplicateCreated = harness.Created(kComponent1, kSurface1);
  const auto duplicateChanged = harness.Changed(kComponent1, kSurface1);
  Expect(duplicateCreated.accepted && !duplicateCreated.stopDecoder &&
             !duplicateCreated.startDecoder,
         "duplicate Created must not rebuild a healthy decoder");
  Expect(duplicateChanged.accepted && !duplicateChanged.stopDecoder &&
             !duplicateChanged.startDecoder,
         "duplicate Changed must not rebuild a healthy decoder");

  harness.Destroyed(kComponent1, kSurface1);
  const auto duplicateDestroyed = harness.Destroyed(kComponent1, kSurface1);
  Expect(!duplicateDestroyed.accepted && harness.starts == 1 &&
             harness.stops == 1,
         "duplicate Destroyed must be ignored");
}

void TestMixedComponentAndSurfaceIdentity() {
  LifecycleHarness harness;
  harness.Created(kComponent2, kSurface2);

  const auto oldComponentCurrentSurface =
      harness.Destroyed(kComponent1, kSurface2);
  Expect(!oldComponentCurrentSurface.accepted && harness.decoder.available,
         "old component with current Surface must not destroy the decoder");

  const auto surfaceChange = harness.Changed(kComponent2, kSurface3);
  Expect(surfaceChange.accepted && surfaceChange.stopDecoder &&
             surfaceChange.startDecoder,
         "current component with a new Surface must rebuild the decoder");
  Expect(harness.lifecycle.surface() == kSurface3,
         "changed Surface must become current");

  const auto currentComponentOldSurface =
      harness.Destroyed(kComponent2, kSurface2);
  Expect(!currentComponentOldSurface.accepted &&
             harness.lifecycle.surface() == kSurface3 &&
             harness.decoder.available,
         "current component with old Surface must not destroy the decoder");

  const auto staleChange = harness.Changed(kComponent1, kSurface3);
  Expect(!staleChange.accepted && harness.lifecycle.component() == kComponent2,
         "old component Changed callback must not take ownership");
}

void TestForegroundBackgroundSurfaceInterleaving() {
  LifecycleHarness harness;
  harness.Created(kComponent1, kSurface1);
  const auto background = harness.Background();
  Expect(background.accepted && background.stopDecoder &&
             !background.startDecoder && !harness.decoder.available,
         "background must stop and clear the decoder");

  const auto backgroundReplacement =
      harness.Created(kComponent2, kSurface2);
  Expect(backgroundReplacement.accepted &&
             !backgroundReplacement.startDecoder &&
             harness.lifecycle.surface() == kSurface2,
         "background Surface replacement must update identity without decoding");
  const auto backgroundChanged = harness.Changed(kComponent2, kSurface3);
  Expect(backgroundChanged.accepted && !backgroundChanged.startDecoder &&
             harness.lifecycle.surface() == kSurface3,
         "background Surface change must remain decoder-free");

  const auto foreground = harness.Foreground();
  Expect(foreground.accepted && foreground.startDecoder &&
             harness.decoder.available,
         "foreground with current Surface must start a decoder");
  const auto duplicateForeground = harness.Foreground();
  Expect(duplicateForeground.accepted &&
             !duplicateForeground.stopDecoder &&
             !duplicateForeground.startDecoder,
         "duplicate foreground must keep a healthy decoder");

  harness.Background();
  const auto destroyedInBackground =
      harness.Destroyed(kComponent2, kSurface3);
  Expect(destroyedInBackground.accepted &&
             !destroyedInBackground.stopDecoder,
         "background destruction must clear Surface without double stop");
  const auto foregroundWithoutSurface = harness.Foreground();
  Expect(foregroundWithoutSurface.accepted &&
             !foregroundWithoutSurface.startDecoder,
         "foreground without Surface must not create a decoder");

  const auto createdAfterForeground =
      harness.Created(kComponent1, kSurface1);
  Expect(createdAfterForeground.accepted &&
             createdAfterForeground.startDecoder,
         "Surface created after foreground must start decoding");
}

void TestSameEpochAuthenticationRequiresCompleteRecovery() {
  DecoderRecoveryCoordinator coordinator;
  coordinator.OnInputSubmitted(DecoderInputKind::kCodecData, true);
  coordinator.OnInputSubmitted(DecoderInputKind::kSyncFrame, true);
  Expect(coordinator.state() == DecoderRecoveryState::kReady,
         "test setup must start with a ready decode chain");

  coordinator.RequireCodecData();
  coordinator.RequestKeyFrame();
  Expect(coordinator.state() == DecoderRecoveryState::kNeedsCodecData,
         "same-epoch authentication must invalidate the old decode chain");
  Expect(!coordinator.InputAllowed(DecoderInputKind::kFrame),
         "ordinary P frames must be rejected immediately after reconnect");
  Expect(coordinator.ConsumeKeyFrameRequest(),
         "same-epoch authentication must request a new codec sync AU");

  coordinator.OnInputSubmitted(DecoderInputKind::kCodecData, true);
  Expect(coordinator.state() == DecoderRecoveryState::kNeedsSyncFrame,
         "complete codec data must advance recovery to the sync-frame gate");
  Expect(!coordinator.InputAllowed(DecoderInputKind::kFrame),
         "P frames must remain rejected before an IDR is submitted");
  coordinator.OnInputSubmitted(DecoderInputKind::kSyncFrame, true);
  Expect(coordinator.state() == DecoderRecoveryState::kReady,
         "a submitted IDR after codec data must complete recovery");
}

void TestFlushClosesNeedInputGateBeforeClearingQueues() {
  DecoderCallbackGate gate;
  gate.SetState(DecoderLifecycleState::kRunning);
  std::deque<int> inputSlots{1};
  bool callbackAdmittedInsideFlush = false;

  gate.BeginFlush([&] {
    // Simulate NeedInput arriving after FlushDecoder has entered its critical
    // window but before the old queue is cleared.
    callbackAdmittedInsideFlush = gate.CallbacksAllowed();
    if (callbackAdmittedInsideFlush) {
      inputSlots.push_back(2);
    }
    inputSlots.clear();
  });
  Expect(!callbackAdmittedInsideFlush,
         "NeedInput in the flush window must observe a closed callback gate");
  Expect(inputSlots.empty(),
         "all pre-flush input slots must be cleared");

  // Simulate a callback that passed an optimistic check before the gate closed,
  // then resumed after queue clearing. The production callback performs the
  // same second gate check while holding decoder_queue_mutex_.
  const bool callbackPrecheckedWhileRunning = true;
  if (callbackPrecheckedWhileRunning && gate.CallbacksAllowed()) {
    inputSlots.push_back(3);
  }
  Expect(inputSlots.empty(),
         "a prechecked NeedInput callback must not revive a stale slot");
  Expect(gate.state() == DecoderLifecycleState::kFlushing,
         "the callback gate must remain closed until decoder restart");
}

void TestAvcRecoveryRequiresSpsPpsAndIdrInOneAccessUnit() {
  const auto complete = CompleteRecoveryAccessUnit();
  const auto recovery = hss::receiver::SplitAvcRecoveryInput(complete);
  Expect(recovery.complete() && !recovery.codecData.empty() &&
             !recovery.syncFrame.empty(),
         "SPS + PPS + IDR must form one complete recovery input");

  auto repeatedCodecData = complete;
  repeatedCodecData.insert(
      repeatedCodecData.begin() + 8, complete.begin(), complete.begin() + 8);
  Expect(hss::receiver::SplitAvcRecoveryInput(repeatedCodecData).complete(),
         "repeated SPS data must not prevent a complete SPS/PPS/IDR recovery");

  const std::vector<std::byte> codecOnly(
      complete.begin(), complete.begin() + 15);
  const std::vector<std::byte> idrOnly(
      complete.begin() + 15, complete.end());
  Expect(!hss::receiver::SplitAvcRecoveryInput(codecOnly).complete(),
         "SPS/PPS without IDR must remain in recovery");
  Expect(!hss::receiver::SplitAvcRecoveryInput(idrOnly).complete(),
         "a later fragmented IDR without SPS/PPS must remain in recovery");
}

void TestDecodeQueueOverflowInvalidatesDependencies() {
  DecoderRecoveryCoordinator coordinator;
  coordinator.OnInputSubmitted(DecoderInputKind::kCodecData, true);
  coordinator.OnInputSubmitted(DecoderInputKind::kSyncFrame, true);
  std::deque<DecoderInputKind> pending{
      DecoderInputKind::kFrame,
      DecoderInputKind::kFrame,
      DecoderInputKind::kFrame};

  bool overflowAccessUnitSubmitted = false;
  const auto decision = coordinator.Admit(
      pending.size(), 3, [&] { pending.clear(); });
  if (decision.accepted) {
    overflowAccessUnitSubmitted = true;
    pending.push_back(DecoderInputKind::kFrame);
  }
  Expect(!decision.accepted && decision.clearPending &&
             decision.requestKeyFrame &&
             decision.nextState == DecoderRecoveryState::kNeedsCodecData,
         "decode queue overflow must clear dependencies and require codec sync");
  Expect(!overflowAccessUnitSubmitted && pending.empty(),
         "the AU that triggers overflow must not be submitted");
  Expect(!coordinator.InputAllowed(DecoderInputKind::kFrame),
         "P frames after overflow must not be submitted");
  Expect(coordinator.ConsumeKeyFrameRequest(),
         "queue overflow must request a complete codec sync AU");

  const auto recovery = hss::receiver::SplitAvcRecoveryInput(
      CompleteRecoveryAccessUnit());
  Expect(recovery.complete(),
         "overflow recovery must use one complete SPS/PPS/IDR access unit");
  coordinator.OnInputSubmitted(DecoderInputKind::kCodecData, true);
  Expect(!coordinator.InputAllowed(DecoderInputKind::kFrame),
         "P frames must remain blocked after SPS/PPS submission");
  coordinator.OnInputSubmitted(DecoderInputKind::kSyncFrame, true);
  Expect(coordinator.state() == DecoderRecoveryState::kReady,
         "complete codec data and IDR must recover after overflow");
}

void TestNormalDecodeQueueAdmissionDoesNotRegressPlayback() {
  DecoderRecoveryCoordinator coordinator;
  coordinator.OnInputSubmitted(DecoderInputKind::kCodecData, true);
  coordinator.OnInputSubmitted(DecoderInputKind::kSyncFrame, true);
  bool cleared = false;
  const auto decision = coordinator.Admit(
      2, 3, [&] { cleared = true; });
  Expect(decision.accepted && !decision.clearPending &&
             !decision.requestKeyFrame &&
             decision.nextState == DecoderRecoveryState::kReady,
         "normal queue admission must preserve ready playback");
  Expect(!cleared && coordinator.InputAllowed(DecoderInputKind::kFrame),
         "normal continuous P frames must remain allowed");
}

void TestVideoContainAxisUsesActualContainerAspectRatio() {
  using hss::receiver::ShouldFitVideoByWidth;
  Expect(!ShouldFitVideoByWidth(4, 3, 1600, 1000),
         "4:3 in a 16:10 fullscreen container must fit by height");
  Expect(ShouldFitVideoByWidth(16, 9, 1600, 1000),
         "16:9 in a 16:10 fullscreen container must fit by width");
  Expect(!ShouldFitVideoByWidth(9, 16, 1600, 1000),
         "portrait video in a landscape container must fit by height");
  Expect(ShouldFitVideoByWidth(9, 16, 900, 2000),
         "portrait video in a narrower portrait container must fit by width");
  Expect(!ShouldFitVideoByWidth(4, 3, 16, 9),
         "embedded 16:9 playback must keep 4:3 video height-bound");
  Expect(!ShouldFitVideoByWidth(16, 10, 16, 9),
         "embedded 16:9 playback must keep 16:10 video height-bound");
  Expect(ShouldFitVideoByWidth(21, 9, 16, 9),
         "embedded 16:9 playback must keep ultrawide video width-bound");
  Expect(!ShouldFitVideoByWidth(0, 3, 16, 9),
         "invalid dimensions must never select width-bound layout");
}

}  // namespace

int main() {
  TestNewSurfaceCreatedBeforeOldDestroyed();
  TestOldSurfaceDestroyedBeforeNewCreated();
  TestDuplicateCallbacksAreIdempotent();
  TestMixedComponentAndSurfaceIdentity();
  TestForegroundBackgroundSurfaceInterleaving();
  TestSameEpochAuthenticationRequiresCompleteRecovery();
  TestFlushClosesNeedInputGateBeforeClearingQueues();
  TestAvcRecoveryRequiresSpsPpsAndIdrInOneAccessUnit();
  TestDecodeQueueOverflowInvalidatesDependencies();
  TestNormalDecodeQueueAdmissionDoesNotRegressPlayback();
  TestVideoContainAxisUsesActualContainerAspectRatio();
  std::cout << "Receiver lifecycle state transition tests passed.\n";
  return 0;
}
