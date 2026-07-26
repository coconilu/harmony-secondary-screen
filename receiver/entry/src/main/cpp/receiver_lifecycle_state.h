#pragma once

#include "decoder_state.h"

#include <cstdint>

namespace hss::receiver {

struct DecoderRuntimeSnapshot final {
  DecoderLifecycleState state = DecoderLifecycleState::kStopped;
  bool available = false;
};

struct ReceiverLifecycleDecision final {
  bool accepted = false;
  bool stopDecoder = false;
  bool startDecoder = false;
};

class ReceiverLifecycleState final {
 public:
  ReceiverLifecycleDecision SurfaceCreated(std::uintptr_t component,
                                           std::uintptr_t surface,
                                           DecoderRuntimeSnapshot decoder) {
    if (component == 0 || surface == 0) return {};
    const bool identityChanged = component != component_ || surface != surface_;
    component_ = component;
    surface_ = surface;
    return Reconcile(true, identityChanged, decoder);
  }

  ReceiverLifecycleDecision SurfaceChanged(std::uintptr_t component,
                                           std::uintptr_t surface,
                                           DecoderRuntimeSnapshot decoder) {
    if (component == 0 || surface == 0 ||
        (component_ != 0 && component != component_)) {
      return {};
    }
    const bool identityChanged = component != component_ || surface != surface_;
    component_ = component;
    surface_ = surface;
    return Reconcile(true, identityChanged, decoder);
  }

  ReceiverLifecycleDecision SurfaceDestroyed(std::uintptr_t component,
                                             std::uintptr_t surface,
                                             DecoderRuntimeSnapshot decoder) {
    if (component_ == 0 || surface_ == 0 || component != component_ ||
        surface != surface_) {
      return {};
    }
    component_ = 0;
    surface_ = 0;
    return Reconcile(true, true, decoder);
  }

  ReceiverLifecycleDecision Foreground(DecoderRuntimeSnapshot decoder) {
    foreground_ = true;
    return Reconcile(true, false, decoder);
  }

  ReceiverLifecycleDecision Background(DecoderRuntimeSnapshot decoder) {
    foreground_ = false;
    return Reconcile(true, false, decoder);
  }

  bool DecoderShouldRun() const { return foreground_ && surface_ != 0; }
  std::uintptr_t component() const { return component_; }
  std::uintptr_t surface() const { return surface_; }
  bool foreground() const { return foreground_; }

 private:
  ReceiverLifecycleDecision Reconcile(bool accepted, bool identityChanged,
                                      DecoderRuntimeSnapshot decoder) const {
    const bool decoderExists =
        decoder.available || decoder.state != DecoderLifecycleState::kStopped;
    const bool decoderHealthy =
        decoder.available && decoder.state == DecoderLifecycleState::kRunning;
    const bool shouldRun = DecoderShouldRun();
    return {
        accepted,
        decoderExists && (!shouldRun || identityChanged || !decoderHealthy),
        shouldRun && (identityChanged || !decoderHealthy),
    };
  }

  bool foreground_ = true;
  std::uintptr_t component_ = 0;
  std::uintptr_t surface_ = 0;
};

}  // namespace hss::receiver
