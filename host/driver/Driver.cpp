#include "Driver.h"

#include "../graphics/com_apartment.h"

#include <avrt.h>

#include <algorithm>
#include <array>
#include <vector>

#ifndef RETURN_IF_FAILED
#define RETURN_IF_FAILED(expression)             \
  do {                                           \
    const HRESULT hss_result = (expression);     \
    if (FAILED(hss_result)) return hss_result;   \
  } while (false)
#endif

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1200;
constexpr std::uint32_t kFps = 60;
constexpr std::uint32_t kBitrate = 16'000'000;
constexpr GUID kMonitorContainerId = {0x75ef52a4,
                                      0x40d6,
                                      0x47ca,
                                      {0xa9, 0x16, 0x55, 0xa2, 0xa2, 0x99, 0x41, 0x5f}};

struct DeviceContextWrapper {
  hss::driver::DeviceContext* value = nullptr;
};

struct MonitorContextWrapper {
  hss::driver::MonitorContext* value = nullptr;
};

WDF_DECLARE_CONTEXT_TYPE(DeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* mode, DWORD width, DWORD height,
                    DWORD refresh, bool monitorMode) {
  mode->totalSize.cx = mode->activeSize.cx = width;
  mode->totalSize.cy = mode->activeSize.cy = height;
  mode->vSyncFreq = {refresh, 1};
  mode->hSyncFreq = {refresh * height, 1};
  mode->pixelRate = static_cast<UINT64>(refresh) * width * height;
  mode->scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  mode->AdditionalSignalInfo.videoStandard = 255;
  mode->AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
}

IDDCX_MONITOR_MODE MonitorMode() {
  IDDCX_MONITOR_MODE mode{};
  mode.Size = sizeof(mode);
  mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
  FillSignalInfo(&mode.MonitorVideoSignalInfo, kWidth, kHeight, kFps, true);
  return mode;
}

IDDCX_TARGET_MODE TargetMode() {
  IDDCX_TARGET_MODE mode{};
  mode.Size = sizeof(mode);
  FillSignalInfo(&mode.TargetVideoSignalInfo.targetVideoSignalInfo, kWidth, kHeight, kFps, false);
  return mode;
}

}  // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD HssDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY HssDeviceD0Entry;
EVT_IDD_CX_ADAPTER_INIT_FINISHED HssAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES HssAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION HssParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES HssGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES HssQueryTargetModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN HssAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN HssUnassignSwapChain;

extern "C" BOOL WINAPI DllMain(HINSTANCE, UINT, LPVOID) {
  return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, HssDeviceAdd);
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
  return WdfDriverCreate(driverObject, registryPath, &attributes, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS HssDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit) {
  UNREFERENCED_PARAMETER(driver);
  WDF_PNPPOWER_EVENT_CALLBACKS powerCallbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&powerCallbacks);
  powerCallbacks.EvtDeviceD0Entry = HssDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &powerCallbacks);

  IDD_CX_CLIENT_CONFIG iddConfig;
  IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);
  iddConfig.EvtIddCxAdapterInitFinished = HssAdapterInitFinished;
  iddConfig.EvtIddCxAdapterCommitModes = HssAdapterCommitModes;
  iddConfig.EvtIddCxParseMonitorDescription = HssParseMonitorDescription;
  iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = HssGetDefaultModes;
  iddConfig.EvtIddCxMonitorQueryTargetModes = HssQueryTargetModes;
  iddConfig.EvtIddCxMonitorAssignSwapChain = HssAssignSwapChain;
  iddConfig.EvtIddCxMonitorUnassignSwapChain = HssUnassignSwapChain;
  NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfig);
  if (!NT_SUCCESS(status)) return status;

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
  attributes.EvtCleanupCallback = [](WDFOBJECT object) {
    auto* wrapper = WdfObjectGet_DeviceContextWrapper(object);
    delete wrapper->value;
    wrapper->value = nullptr;
  };
  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&deviceInit, &attributes, &device);
  if (!NT_SUCCESS(status)) return status;
  status = IddCxDeviceInitialize(device);
  if (!NT_SUCCESS(status)) return status;
  WdfObjectGet_DeviceContextWrapper(device)->value = new hss::driver::DeviceContext(device);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS HssDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState) {
  UNREFERENCED_PARAMETER(previousState);
  WdfObjectGet_DeviceContextWrapper(device)->value->InitializeAdapter();
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS HssAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
  if (!NT_SUCCESS(args->AdapterInitStatus)) return args->AdapterInitStatus;
  WdfObjectGet_DeviceContextWrapper(adapter)->value->FinishAdapterInitialization();
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS HssAdapterCommitModes(IDDCX_ADAPTER adapter,
                               const IDARG_IN_COMMITMODES* args) {
  UNREFERENCED_PARAMETER(adapter);
  UNREFERENCED_PARAMETER(args);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS HssParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* args,
                                    IDARG_OUT_PARSEMONITORDESCRIPTION* output) {
  UNREFERENCED_PARAMETER(args);
  UNREFERENCED_PARAMETER(output);
  return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS HssGetDefaultModes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* args,
                            IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* output) {
  UNREFERENCED_PARAMETER(monitor);
  output->DefaultMonitorModeBufferOutputCount = 1;
  output->PreferredMonitorModeIdx = 0;
  if (args->DefaultMonitorModeBufferInputCount >= 1) {
    args->pDefaultMonitorModes[0] = MonitorMode();
  }
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS HssQueryTargetModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* args,
                             IDARG_OUT_QUERYTARGETMODES* output) {
  UNREFERENCED_PARAMETER(monitor);
  output->TargetModeBufferOutputCount = 1;
  if (args->TargetModeBufferInputCount >= 1) {
    args->pTargetModes[0] = TargetMode();
  }
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS HssAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* args) {
  WdfObjectGet_MonitorContextWrapper(monitor)->value->AssignSwapChain(
      args->hSwapChain, args->RenderAdapterLuid, args->hNextSurfaceAvailable);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS HssUnassignSwapChain(IDDCX_MONITOR monitor) {
  WdfObjectGet_MonitorContextWrapper(monitor)->value->UnassignSwapChain();
  return STATUS_SUCCESS;
}

namespace hss::driver {

Direct3DDevice::Direct3DDevice(LUID adapterLuid) : adapter_luid_(adapterLuid) {}

HRESULT Direct3DDevice::Initialize() {
  RETURN_IF_FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)));
  RETURN_IF_FAILED(factory_->EnumAdapterByLuid(adapter_luid_, IID_PPV_ARGS(&adapter_)));
  RETURN_IF_FAILED(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
  ComPtr<ID3D11Multithread> multithread;
  if (SUCCEEDED(context.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }
  return S_OK;
}

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain,
                                       std::shared_ptr<Direct3DDevice> device,
                                       HANDLE newFrameEvent)
    : swap_chain_(swapChain), device_(std::move(device)), new_frame_event_(newFrameEvent) {
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  acquire_thread_ = std::thread(&SwapChainProcessor::AcquireLoop, this);
  encoder_thread_ = std::thread(&SwapChainProcessor::EncoderLoop, this);
}

SwapChainProcessor::~SwapChainProcessor() {
  {
    std::scoped_lock lock(queue_mutex_);
    stopping_ = true;
  }
  SetEvent(stop_event_);
  queue_ready_.notify_all();
  if (acquire_thread_.joinable()) acquire_thread_.join();
  if (encoder_thread_.joinable()) encoder_thread_.join();
  if (keyframe_event_ != nullptr) CloseHandle(keyframe_event_);
  if (stop_event_ != nullptr) CloseHandle(stop_event_);
}

std::uint64_t SwapChainProcessor::ClockMicroseconds() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<std::uint64_t>((counter.QuadPart * 1'000'000LL) / frequency.QuadPart);
}

bool SwapChainProcessor::CopyForEncoder(ID3D11Texture2D* source, PendingFrame* frame) {
  D3D11_TEXTURE2D_DESC desc{};
  source->GetDesc(&desc);
  if (desc.Width != kWidth || desc.Height != kHeight) return false;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = 0;
  desc.MiscFlags = 0;
  if (FAILED(device_->device->CreateTexture2D(&desc, nullptr, &frame->texture))) return false;
  device_->context->CopyResource(frame->texture.Get(), source);
  frame->timestampUs = ClockMicroseconds();
  frame->number = next_frame_.fetch_add(1);
  return true;
}

void SwapChainProcessor::AcquireLoop() {
  DWORD taskIndex = 0;
  HANDLE avTask = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);
  ComPtr<IDXGIDevice> dxgiDevice;
  if (FAILED(device_->device.As(&dxgiDevice))) return;
  IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
  setDevice.pDevice = dxgiDevice.Get();
  if (FAILED(IddCxSwapChainSetDevice(swap_chain_, &setDevice))) return;

  while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
    const HRESULT result = IddCxSwapChainReleaseAndAcquireBuffer(swap_chain_, &buffer);
    if (result == E_PENDING) {
      const std::array<HANDLE, 2> events{new_frame_event_, stop_event_};
      WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, 16);
      continue;
    }
    if (FAILED(result)) break;
    ComPtr<IDXGIResource> resource;
    resource.Attach(buffer.MetaData.pSurface);
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource.As(&texture))) {
      PendingFrame frame;
      if (CopyForEncoder(texture.Get(), &frame)) {
        std::scoped_lock lock(queue_mutex_);
        while (queue_.size() >= 2) queue_.pop_front();
        queue_.push_back(std::move(frame));
        queue_ready_.notify_one();
      }
    }
    resource.Reset();
    if (FAILED(IddCxSwapChainFinishedProcessingFrame(swap_chain_))) break;
  }
  WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swap_chain_));
  swap_chain_ = nullptr;
  if (avTask != nullptr) AvRevertMmThreadCharacteristics(avTask);
}

void SwapChainProcessor::EncoderLoop() {
  graphics::ComMtaApartment comApartment;
  if (!comApartment.ready()) return;
  graphics::MfH264Encoder encoder;
  if (FAILED(encoder.Initialize(device_->device.Get(), kWidth, kHeight, kFps, kBitrate))) return;
  graphics::PipeSink sink;
  for (;;) {
    PendingFrame frame;
    {
      std::unique_lock lock(queue_mutex_);
      queue_ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty()) break;
      frame = std::move(queue_.back());
      queue_.clear();
    }
    // The service can start after the driver; retry until its event exists.
    if (keyframe_event_ == nullptr) {
      keyframe_event_ = OpenEventW(SYNCHRONIZE, FALSE,
                                   L"Global\\HarmonySecondaryScreen.RequestKeyframe");
    }
    bool forceKeyframe = false;
    if (keyframe_event_ != nullptr) {
      const DWORD waitResult = WaitForSingleObject(keyframe_event_, 0);
      forceKeyframe = waitResult == WAIT_OBJECT_0;
      if (waitResult == WAIT_FAILED) {
        CloseHandle(keyframe_event_);
        keyframe_event_ = nullptr;
      }
    }
    graphics::EncodedFrame encoded;
    const HRESULT result = encoder.Encode(frame.texture.Get(), frame.timestampUs,
                                          forceKeyframe, &encoded);
    if (result == S_OK && !encoded.bytes.empty()) {
      sink.Write(frame.number, encoded, encoded.keyframe);
    }
  }
}

void MonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter,
                                     HANDLE newFrameEvent) {
  processor_.reset();
  auto d3d = std::make_shared<Direct3DDevice>(renderAdapter);
  if (FAILED(d3d->Initialize())) {
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));
    return;
  }
  processor_ = std::make_unique<SwapChainProcessor>(swapChain, std::move(d3d), newFrameEvent);
}

void MonitorContext::UnassignSwapChain() {
  processor_.reset();
}

void DeviceContext::InitializeAdapter() {
  IDDCX_ADAPTER_CAPS caps{};
  caps.Size = sizeof(caps);
  caps.MaxMonitorsSupported = 1;
  caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_NETWORK_OTHER;
  caps.EndPointDiagnostics.pEndPointFriendlyName = L"Harmony Secondary Screen";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"Harmony Secondary Screen";
  caps.EndPointDiagnostics.pEndPointModelName = L"Wi-Fi 1920x1200";
  IDDCX_ENDPOINT_VERSION version{};
  version.Size = sizeof(version);
  version.MajorVer = 0;
  version.MinorVer = 1;
  caps.EndPointDiagnostics.pFirmwareVersion = &version;
  caps.EndPointDiagnostics.pHardwareVersion = &version;

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
  IDARG_IN_ADAPTER_INIT input{};
  input.WdfDevice = device_;
  input.pCaps = &caps;
  input.ObjectAttributes = &attributes;
  IDARG_OUT_ADAPTER_INIT output{};
  if (NT_SUCCESS(IddCxAdapterInitAsync(&input, &output))) {
    adapter_ = output.AdapterObject;
    WdfObjectGet_DeviceContextWrapper(adapter_)->value = this;
  }
}

void DeviceContext::FinishAdapterInitialization() {
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContextWrapper);
  attributes.EvtCleanupCallback = [](WDFOBJECT object) {
    auto* wrapper = WdfObjectGet_MonitorContextWrapper(object);
    delete wrapper->value;
    wrapper->value = nullptr;
  };
  IDDCX_MONITOR_INFO info{};
  info.Size = sizeof(info);
  info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
  info.ConnectorIndex = 0;
  info.MonitorDescription.Size = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = 0;
  info.MonitorDescription.pData = nullptr;
  info.MonitorContainerId = kMonitorContainerId;
  IDARG_IN_MONITORCREATE input{};
  input.ObjectAttributes = &attributes;
  input.pMonitorInfo = &info;
  IDARG_OUT_MONITORCREATE output{};
  if (!NT_SUCCESS(IddCxMonitorCreate(adapter_, &input, &output))) return;
  WdfObjectGet_MonitorContextWrapper(output.MonitorObject)->value =
      new MonitorContext(output.MonitorObject);
  IDARG_OUT_MONITORARRIVAL arrival{};
  IddCxMonitorArrival(output.MonitorObject, &arrival);
}

}  // namespace hss::driver
