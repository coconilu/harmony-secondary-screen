#pragma once

#include "bounded_control_queue.h"
#include "decoder_orchestration.h"
#include "decoder_state.h"
#include "native_protocol.h"
#include "receiver_lifecycle_state.h"
#include "websocket_protocol.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hss::receiver {

struct StatusSnapshot {
  std::string state;
  std::string detail;
  std::string listenAddress;
  std::string pairedAddress;
  std::string deviceId;
  bool listening = false;
  bool connected = false;
  bool paired = false;
  std::uint64_t framesDecoded = 0;
  std::uint64_t framesDropped = 0;
  std::uint64_t receivedFrames = 0;
};

struct PairingRecord {
  std::string deviceId;
  std::string senderId;
  std::string credential;
  std::uint64_t version = 0;
};

class ReceiverSession final {
 public:
  static ReceiverSession& Instance();
  ~ReceiverSession();
  ReceiverSession(const ReceiverSession&) = delete;
  ReceiverSession& operator=(const ReceiverSession&) = delete;

  bool Start(std::string listenAddress);
  void Stop();
  bool ConfigureTrust(std::string deviceId, std::string senderId, std::string credential,
                      std::uint64_t version);
  bool AuthorizeQr(std::string sessionId, std::string token, std::int64_t expiresAtMs);
  bool AuthorizeShortCode(std::string shortCode);
  void ForgetDevice();
  StatusSnapshot Status() const;
  PairingRecord Pairing() const;
  std::vector<std::string> WifiAddresses() const;

  void OnSurfaceCreated(OH_NativeXComponent* component, void* window);
  void OnSurfaceChanged(OH_NativeXComponent* component, void* window);
  void OnSurfaceDestroyed(OH_NativeXComponent* component, void* window);
  void OnAppForeground();
  void OnAppBackground();

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
  void NetworkLoop();
  bool OpenListener();
  bool AcceptWebSocket();
  bool ReadUpgrade(int client);
  bool AuthenticateConnection();
  bool RunConnectedSession();
  bool SendControl(std::string_view json);
  bool HandleControl(std::string_view json);
  void HandleVideo(const std::byte* data, std::size_t size);
  void RequestKeyframe();
  void CloseControlSocket();
  void CloseSockets();
  void SetState(std::string state, std::string detail, bool listening, bool connected);

  DecoderRuntimeSnapshot DecoderRuntimeLocked() const;
  bool ApplyLifecycleDecisionLocked(ReceiverLifecycleDecision decision);
  bool CreateDecoderLocked();
  void DestroyDecoderLocked();
  void ClearDecoderQueues();
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
  std::string detail_ = "正在检查当前 Wi-Fi";
  std::string listen_address_;
  std::string paired_address_;
  bool listening_ = false;
  bool connected_ = false;
  std::string device_id_;
  std::string trusted_sender_id_;
  std::string trusted_credential_;
  std::uint64_t pairing_record_version_ = 0;
  std::string pending_session_id_;
  std::string pending_token_;
  std::string pending_short_code_;
  std::string consumed_session_id_;
  std::chrono::system_clock::time_point pairing_expires_at_;
  std::uint32_t latest_source_epoch_ = 0;
  std::uint32_t active_source_epoch_ = 0;
  std::atomic<bool> desired_{false};
  std::thread worker_;
  std::atomic<int> listener_socket_{-1};
  std::atomic<int> control_socket_{-1};
  std::timed_mutex send_mutex_;
  BoundedControlQueue telemetry_queue_{8};
  websocket::Decoder websocket_decoder_;
  std::atomic<std::uint64_t> received_frames_{0};
  std::atomic<std::uint64_t> received_bytes_{0};
  std::atomic<std::uint64_t> decoder_resync_events_{0};
  std::atomic<std::uint64_t> keyframe_requests_sent_{0};

  std::mutex decoder_lifecycle_mutex_;
  std::mutex decoder_queue_mutex_;
  DecoderCallbackGate decoder_callback_gate_;
  std::atomic<OH_AVCodec*> decoder_{nullptr};
  DecoderRecoveryCoordinator decoder_recovery_;
  ReceiverLifecycleState lifecycle_state_;
  std::deque<InputSlot> input_slots_;
  std::deque<DecodedInput> decode_queue_;
  std::atomic<std::uint64_t> frames_decoded_{0};
  std::atomic<std::uint64_t> frames_dropped_{0};
};

}  // namespace hss::receiver
