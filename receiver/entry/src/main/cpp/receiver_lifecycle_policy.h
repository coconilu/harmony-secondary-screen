#pragma once

#include "decoder_state.h"

#include <cstdint>

namespace hss::receiver {

constexpr bool ShouldReplaceSurface(std::uintptr_t currentSurface,
                                    std::uintptr_t callbackSurface) {
  return callbackSurface != 0 && callbackSurface != currentSurface;
}

constexpr bool ShouldDestroyCurrentSurface(std::uintptr_t currentSurface,
                                           std::uintptr_t callbackSurface) {
  return currentSurface != 0 && callbackSurface == currentSurface;
}

constexpr bool ShouldAcceptSurfaceChange(std::uintptr_t currentComponent,
                                         std::uintptr_t callbackComponent) {
  return currentComponent == 0 || callbackComponent == currentComponent;
}

constexpr bool DecoderShouldRun(bool appForeground, bool surfaceAvailable) {
  return appForeground && surfaceAvailable;
}

constexpr bool DecoderRequiresRebuild(bool shouldRun, DecoderLifecycleState state,
                                      bool decoderAvailable) {
  return shouldRun &&
         (state != DecoderLifecycleState::kRunning || !decoderAvailable);
}

}  // namespace hss::receiver
