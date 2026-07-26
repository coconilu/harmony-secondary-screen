#include "../entry/src/main/cpp/receiver_lifecycle_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  using hss::receiver::DecoderLifecycleState;

  Expect(!hss::receiver::ShouldReplaceSurface(0x1000, 0x1000),
         "same Surface must not rebuild the decoder");
  Expect(hss::receiver::ShouldReplaceSurface(0x1000, 0x2000),
         "a new Surface must replace the old Surface");
  Expect(!hss::receiver::ShouldReplaceSurface(0x1000, 0),
         "a null Surface callback must not replace the current Surface");

  Expect(hss::receiver::ShouldDestroyCurrentSurface(0x2000, 0x2000),
         "the current Surface destruction must stop the decoder");
  Expect(!hss::receiver::ShouldDestroyCurrentSurface(0x2000, 0x1000),
         "a stale Surface destruction must not stop the current decoder");
  Expect(!hss::receiver::ShouldDestroyCurrentSurface(0x2000, 0),
         "a null destruction callback must not stop the current decoder");
  Expect(hss::receiver::ShouldAcceptSurfaceChange(0x10, 0x10),
         "the current component may update its Surface");
  Expect(!hss::receiver::ShouldAcceptSurfaceChange(0x20, 0x10),
         "a stale component callback must not replace the current Surface");

  Expect(!hss::receiver::DecoderShouldRun(false, true),
         "the decoder must remain stopped in background");
  Expect(!hss::receiver::DecoderShouldRun(true, false),
         "the decoder must remain stopped without a Surface");
  Expect(hss::receiver::DecoderShouldRun(true, true),
         "the decoder may run only in foreground with a Surface");

  Expect(!hss::receiver::DecoderRequiresRebuild(
             true, DecoderLifecycleState::kRunning, true),
         "a healthy decoder must not be rebuilt by duplicate callbacks");
  Expect(hss::receiver::DecoderRequiresRebuild(
             true, DecoderLifecycleState::kStopped, false),
         "a stopped decoder must rebuild when lifecycle prerequisites return");
  Expect(!hss::receiver::DecoderRequiresRebuild(
             false, DecoderLifecycleState::kStopped, false),
         "background lifecycle must not create a decoder");

  std::cout << "Receiver lifecycle policy tests passed.\n";
  return 0;
}
