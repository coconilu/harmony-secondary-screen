#pragma once

#include "decoder_state.h"
#include "native_protocol.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hss::receiver {

struct StatusSnapshot {
  std::string state;
  std::string detail;
  bool connected = false;
  std::uint64_t framesDecoded = 0;
  std::uint64_t framesDropped = 0;
};

class ReceiverSession final {
 public:
  static ReceiverSession& Instance();
  ~ReceiverSession();
  ReceiverSession(const ReceiverSession&) = delete;
  ReceiverSession& operator=(const ReceiverSession&) = delete;

  bool Start(std::string host, std::string pairingCode);
  void Stop();
  StatusSnapshot Status() const;
  void SetInputMode(std::string mode);

  void OnSurfaceCreated(OH_NativeXComponent* component, void* window);
  void OnSurfaceChanged(OH_NativeXComponent* component, void* window);
  void OnSurfaceDestroyed();
  void OnTouch(OH_NativeXComponent* component, void* window);

 private:
  ReceiverSession() = default;

  struct InputSlot {
    std::uint32_t index = 0;
    OH_AVBuffer* buffer = nullptr;
  };
  struct DecodedInput {
    std::vector<std::byte> bytes;
    std::uint64_t timestampUs = 0;
    bool keyframe = false;
  };
  struct Assembly {
    std::uint16_t fragmentCount = 0;
    std::uint16_t flags = 0;
    std::uint64_t timestampUs = 0;
    std::chrono::steady_clock::time_point created;
    std::vector<std::vector<std::byte>> fragments;
    std::vector<bool> received;
    std::size_t receivedCount = 0;
  };

  void NetworkLoop();
  bool ConnectControl(bool resume);
  bool RunConnectedSession();
  bool SendControl(std::string_view json);
  void HandleControl(std::string_view json);
  void HandleVideo(const std::byte* data, std::size_t size);
  void SweepAssemblies();
  void RequestKeyframe();
  void CloseSockets();
  void SetState(std::string state, std::string detail, bool connected);

  bool StartDecoder();
  bool CreateDecoderLocked();
  void DestroyDecoderLocked();
  void ClearDecoderQueues();
  void StopDecoder();
  bool FlushDecoder();
  void SubmitFrame(DecodedInput frame);
  void PumpDecoderLocked(OH_AVCodec* decoder);
  void DecoderError(int32_t errorCode);
  void DecoderNeedInput(OH_AVCodec* decoder, std::uint32_t index, OH_AVBuffer* buffer);
  void DecoderOutput(OH_AVCodec* decoder, std::uint32_t index, OH_AVBuffer* buffer);

  static void OnCodecError(OH_AVCodec*, int32_t errorCode, void* userData);
  static void OnCodecStreamChanged(OH_AVCodec*, OH_AVFormat*, void*) {}
  static void OnCodecNeedInput(OH_AVCodec*, std::uint32_t index, OH_AVBuffer* buffer,
                               void* userData);
  static void OnCodecOutput(OH_AVCodec*, std::uint32_t index, OH_AVBuffer* buffer,
                            void* userData);

  mutable std::mutex state_mutex_;
  std::string state_ = "idle";
  std::string detail_ = "等待连接";
  bool connected_ = false;
  std::string host_;
  std::string pairing_code_;
  std::string session_id_;
  std::uint32_t session_short_ = 0;
  std::atomic<bool> desired_{false};
  std::thread worker_;
  std::atomic<int> control_socket_{-1};
  std::atomic<int> video_socket_{-1};
  std::mutex send_mutex_;
  protocol::ControlDecoder control_decoder_;
  std::map<std::uint32_t, Assembly> assemblies_;

  std::mutex decoder_lifecycle_mutex_;
  std::mutex decoder_queue_mutex_;
  std::atomic<DecoderLifecycleState> decoder_state_{DecoderLifecycleState::kStopped};
  std::atomic<OH_AVCodec*> decoder_{nullptr};
  std::atomic<bool> needs_codec_config_{true};
  void* native_window_ = nullptr;
  std::deque<InputSlot> input_slots_;
  std::deque<DecodedInput> decode_queue_;
  std::atomic<std::uint64_t> frames_decoded_{0};
  std::atomic<std::uint64_t> frames_dropped_{0};
  std::atomic<std::int64_t> host_clock_offset_us_{0};

  std::mutex input_mutex_;
  std::string input_mode_ = "pointer";
  float previous_touch_y_ = 0.0F;
  bool touch_active_ = false;
};

}  // namespace hss::receiver
