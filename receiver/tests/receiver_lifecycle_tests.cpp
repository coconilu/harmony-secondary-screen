#include "../entry/src/main/cpp/receiver_lifecycle_state.h"

#include <cstdlib>
#include <iostream>

namespace {

using hss::receiver::DecoderLifecycleState;
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

}  // namespace

int main() {
  TestNewSurfaceCreatedBeforeOldDestroyed();
  TestOldSurfaceDestroyedBeforeNewCreated();
  TestDuplicateCallbacksAreIdempotent();
  TestMixedComponentAndSurfaceIdentity();
  TestForegroundBackgroundSurfaceInterleaving();
  std::cout << "Receiver lifecycle state transition tests passed.\n";
  return 0;
}
