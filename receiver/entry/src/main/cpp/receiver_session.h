#pragma once

#include "bounded_control_queue.h"
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
  std::string listenAddress;
  std::string pairingCode;
  std::string pairedAddress;
  bool listening = false;
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

  bool Start(std::string listenAddress);
  void Stop();
  StatusSnapshot Status() const;

  void OnSurfaceCreated(OH_NativeXComponent* component, void* window);
  void OnSurfaceChanged(OH_NativeXComponent* component, void* window);
  void OnSurfaceDestroyed();

 private:
  ReceiverSession() = default;

  struct InputSlot {
    std::uint32_t index = 0;
    OH_AVBuffer* buffer = nullptr;
  };
  struct DecodedInput {
    std::vector<std::byte> bytes;
    std::uint64_t timestampUs = 0;
    DecoderInputKind kind = DecoderInputKind::kFrame;
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
  bool OpenListeners();
  bool AcceptAndPair();
  bool RunConnectedSession();
  bool SendControl(std::string_view json);
  bool HandleControl(std::string_view json);
  void HandleVideo(const std::byte* data, std::size_t size);
  void SweepAssemblies();
  void RequestKeyframe();
  void CloseControlSocket();
  void CloseSockets();
  void SetState(std::string state, std::string detail, bool listening, bool connected);

  bool StartDecoder();
  bool CreateDecoderLocked();
  void DestroyDecoderLocked();
  void ClearDecoderQueues();
  void StopDecoder();
  bool FlushDecoder();
  void SubmitFrame(DecodedInput frame);
  bool SubmitRecovery(DecodedInput codecData, DecodedInput syncFrame);
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
  std::string detail_ = "请输入本机 Wi-Fi IPv4";
  std::string listen_address_;
  std::string paired_address_;
  bool listening_ = false;
  bool connected_ = false;
  std::string pairing_code_;
  std::string receiver_nonce_;
  std::string session_id_;
  std::uint32_t session_short_ = 0;
  std::chrono::steady_clock::time_point pairing_expires_at_;
  std::atomic<bool> desired_{false};
  std::thread worker_;
  std::atomic<int> listener_socket_{-1};
  std::atomic<int> control_socket_{-1};
  std::atomic<int> video_socket_{-1};
  std::timed_mutex send_mutex_;
  BoundedControlQueue telemetry_queue_{8};
  std::atomic<bool> keyframe_request_pending_{false};
  protocol::ControlDecoder control_decoder_;
  std::map<std::uint32_t, Assembly> assemblies_;

  std::mutex decoder_lifecycle_mutex_;
  std::mutex decoder_queue_mutex_;
  std::atomic<DecoderLifecycleState> decoder_state_{DecoderLifecycleState::kStopped};
  std::atomic<OH_AVCodec*> decoder_{nullptr};
  std::atomic<DecoderRecoveryState> decoder_recovery_state_{
      DecoderRecoveryState::kNeedsCodecData};
  void* native_window_ = nullptr;
  std::deque<InputSlot> input_slots_;
  std::deque<DecodedInput> decode_queue_;
  std::atomic<std::uint64_t> frames_decoded_{0};
  std::atomic<std::uint64_t> frames_dropped_{0};
};

}  // namespace hss::receiver
