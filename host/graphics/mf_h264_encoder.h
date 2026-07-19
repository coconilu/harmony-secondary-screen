#pragma once

#include <d3d11_4.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <vector>

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
  HRESULT CreateVideoProcessor();
  HRESULT CreateEncoder(bool hardware);
  HRESULT ConfigureEncoder();
  HRESULT ConvertToNv12(ID3D11Texture2D* source);
  HRESULT DrainOutput(std::uint64_t timestampUs, EncodedFrame* output);
  void RefreshCodecConfig();
  void Shutdown();

  std::mutex mutex_;
  bool mf_started_ = false;
  bool using_hardware_ = false;
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
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_texture_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager_;
  Microsoft::WRL::ComPtr<IMFTransform> encoder_;
  std::vector<std::byte> codec_config_;
};

}  // namespace hss::graphics
