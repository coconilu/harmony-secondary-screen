#pragma once

#include <d3d11_4.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "async_mft_schedule.h"

namespace hss::graphics {

struct EncodedFrame {
  std::vector<std::byte> bytes;
  bool keyframe = false;
  std::uint64_t timestampUs = 0;
};

// Converts IddCx BGRA swap-chain surfaces to NV12 with D3D11 VideoProcessor and
// feeds a low-latency Media Foundation H.264 MFT. Hardware encoders are selected
// first; the Microsoft software MFT is the explicit fallback.
class MfH264Encoder final {
 public:
  MfH264Encoder() = default;
  ~MfH264Encoder();
  MfH264Encoder(const MfH264Encoder&) = delete;
  MfH264Encoder& operator=(const MfH264Encoder&) = delete;

  HRESULT Initialize(ID3D11Device* device, std::uint32_t width, std::uint32_t height,
                     std::uint32_t fps, std::uint32_t bitrate);
  HRESULT Encode(ID3D11Texture2D* bgraTexture, std::uint64_t timestampUs,
                 bool forceKeyframe, EncodedFrame* output);
  bool using_hardware() const noexcept { return using_hardware_; }

 private:
  struct PendingInput {
    Microsoft::WRL::ComPtr<IMFSample> sample;
    std::uint64_t timestampUs = 0;
    bool forceKeyframe = false;
  };

  HRESULT CreateVideoProcessor();
  HRESULT CreateEncoder(bool hardware);
  HRESULT ConfigureEncoder();
  HRESULT ConvertToNv12(ID3D11Texture2D* source,
                         Microsoft::WRL::ComPtr<ID3D11Texture2D>* converted);
  HRESULT EncodeCurrentNv12(ID3D11Texture2D* texture, std::uint64_t timestampUs,
                            bool forceKeyframe, EncodedFrame* output);
  HRESULT CreateInputSample(ID3D11Texture2D* texture, std::uint64_t timestampUs,
                            Microsoft::WRL::ComPtr<IMFSample>* sample);
  HRESULT SubmitSynchronous(Microsoft::WRL::ComPtr<IMFSample> sample,
                            std::uint64_t timestampUs, bool forceKeyframe,
                            EncodedFrame* output);
  HRESULT EnqueueAsynchronous(Microsoft::WRL::ComPtr<IMFSample> sample,
                              std::uint64_t timestampUs, bool forceKeyframe,
                              EncodedFrame* output);
  HRESULT StartAsyncPump();
  void StopAsyncPump();
  void AsyncEventLoop();
  void SetAsyncError(HRESULT error);
  void ForceKeyframe();
  HRESULT FallbackToSoftware();
  HRESULT DrainOutput(std::uint64_t timestampUs, EncodedFrame* output);
  void RefreshCodecConfig();
  void Shutdown();

  std::mutex mutex_;
  bool mf_started_ = false;
  bool using_hardware_ = false;
  bool asynchronous_ = false;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t fps_ = 0;
  std::uint32_t bitrate_ = 0;
  std::uint32_t dxgi_token_ = 0;

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> processor_enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager_;
  Microsoft::WRL::ComPtr<IMFTransform> encoder_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator_;
  std::vector<std::byte> codec_config_;

  std::mutex async_mutex_;
  std::condition_variable async_ready_;
  std::deque<PendingInput> pending_inputs_;
  std::deque<EncodedFrame> ready_outputs_;
  AsyncMftSchedule async_schedule_;
  std::thread async_thread_;
  bool async_stopping_ = false;
  HRESULT async_error_ = S_OK;
};

}  // namespace hss::graphics
