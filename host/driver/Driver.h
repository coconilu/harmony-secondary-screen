#pragma once

#define NOMINMAX
#include <windows.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <d3d11_2.h>
#include <dxgi1_5.h>
#include <wrl.h>

#include "../graphics/mf_h264_encoder.h"
#include "../graphics/pipe_sink.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace hss::driver {

class Direct3DDevice final {
 public:
  explicit Direct3DDevice(LUID adapterLuid);
  HRESULT Initialize();

  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;

 private:
  LUID adapter_luid_{};
  Microsoft::WRL::ComPtr<IDXGIFactory5> factory_;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
};

class SwapChainProcessor final {
 public:
  SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device,
                     HANDLE newFrameEvent);
  ~SwapChainProcessor();
  SwapChainProcessor(const SwapChainProcessor&) = delete;
  SwapChainProcessor& operator=(const SwapChainProcessor&) = delete;

 private:
  struct PendingFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::uint64_t timestampUs = 0;
    std::uint32_t number = 0;
  };

  void AcquireLoop();
  void EncoderLoop();
  bool CopyForEncoder(ID3D11Texture2D* source, PendingFrame* frame);
  static std::uint64_t ClockMicroseconds();

  IDDCX_SWAPCHAIN swap_chain_ = nullptr;
  std::shared_ptr<Direct3DDevice> device_;
  HANDLE new_frame_event_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HANDLE keyframe_event_ = nullptr;
  std::atomic<std::uint32_t> next_frame_{0};
  std::thread acquire_thread_;
  std::thread encoder_thread_;
  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::deque<PendingFrame> queue_;
  bool stopping_ = false;
};

class MonitorContext final {
 public:
  explicit MonitorContext(IDDCX_MONITOR monitor) : monitor_(monitor) {}
  void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
  void UnassignSwapChain();

 private:
  IDDCX_MONITOR monitor_ = nullptr;
  std::unique_ptr<SwapChainProcessor> processor_;
};

class DeviceContext final {
 public:
  explicit DeviceContext(WDFDEVICE device) : device_(device) {}
  void InitializeAdapter();
  void FinishAdapterInitialization();

 private:
  WDFDEVICE device_ = nullptr;
  IDDCX_ADAPTER adapter_ = nullptr;
};

}  // namespace hss::driver
