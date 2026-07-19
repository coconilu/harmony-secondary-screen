#include "mf_h264_encoder.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <propvarutil.h>

#include <algorithm>
#include <array>
#include <cstring>

#ifndef RETURN_IF_FAILED
#define RETURN_IF_FAILED(expression)             \
  do {                                           \
    const HRESULT hss_result = (expression);     \
    if (FAILED(hss_result)) return hss_result;   \
  } while (false)
#endif

namespace hss::graphics {
namespace {

using Microsoft::WRL::ComPtr;

HRESULT SetCodecValue(ICodecAPI* codecApi, const GUID& key, VARIANT& value) {
  return codecApi == nullptr ? E_POINTER : codecApi->SetValue(&key, &value);
}

HRESULT SetMediaTypeCommon(IMFMediaType* type, const GUID& subtype, std::uint32_t width,
                           std::uint32_t height, std::uint32_t fps) {
  RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
  RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype));
  RETURN_IF_FAILED(MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height));
  RETURN_IF_FAILED(MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1));
  RETURN_IF_FAILED(MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
  RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
  return S_OK;
}

bool HasAnnexBStartCode(const std::vector<std::byte>& bytes) {
  return bytes.size() >= 4 && bytes[0] == std::byte{0} && bytes[1] == std::byte{0} &&
         ((bytes[2] == std::byte{1}) ||
          (bytes[2] == std::byte{0} && bytes[3] == std::byte{1}));
}

std::vector<std::byte> NormalizeAccessUnit(const BYTE* data, std::size_t size) {
  std::vector<std::byte> original(size);
  std::memcpy(original.data(), data, size);
  if (HasAnnexBStartCode(original)) {
    return original;
  }
  std::vector<std::byte> annexB;
  std::size_t offset = 0;
  while (offset + 4 <= size) {
    const std::uint32_t nalSize = (static_cast<std::uint32_t>(data[offset]) << 24U) |
                                  (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
                                  (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
                                  static_cast<std::uint32_t>(data[offset + 3]);
    offset += 4;
    if (nalSize == 0 || offset + nalSize > size) {
      return original;
    }
    const std::array<std::byte, 4> startCode{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
    annexB.insert(annexB.end(), startCode.begin(), startCode.end());
    annexB.insert(annexB.end(), reinterpret_cast<const std::byte*>(data + offset),
                  reinterpret_cast<const std::byte*>(data + offset + nalSize));
    offset += nalSize;
  }
  return offset == size ? annexB : original;
}

std::vector<std::byte> NormalizeCodecConfig(const BYTE* data, std::size_t size) {
  if (size < 7 || data[0] != 1) {
    return NormalizeAccessUnit(data, size);
  }
  std::vector<std::byte> annexB;
  std::size_t offset = 6;
  const auto appendNal = [&](std::size_t* cursor) -> bool {
    if (*cursor + 2 > size) return false;
    const std::uint16_t nalSize = static_cast<std::uint16_t>(data[*cursor] << 8U) |
                                  static_cast<std::uint16_t>(data[*cursor + 1]);
    *cursor += 2;
    if (nalSize == 0 || *cursor + nalSize > size) return false;
    const std::array<std::byte, 4> startCode{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
    annexB.insert(annexB.end(), startCode.begin(), startCode.end());
    annexB.insert(annexB.end(), reinterpret_cast<const std::byte*>(data + *cursor),
                  reinterpret_cast<const std::byte*>(data + *cursor + nalSize));
    *cursor += nalSize;
    return true;
  };
  const std::uint8_t spsCount = data[5] & 0x1fU;
  for (std::uint8_t index = 0; index < spsCount; ++index) {
    if (!appendNal(&offset)) return {};
  }
  if (offset >= size) return {};
  const std::uint8_t ppsCount = data[offset++];
  for (std::uint8_t index = 0; index < ppsCount; ++index) {
    if (!appendNal(&offset)) return {};
  }
  return annexB;
}

}  // namespace

MfH264Encoder::~MfH264Encoder() {
  Shutdown();
}

HRESULT MfH264Encoder::Initialize(ID3D11Device* device, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t fps,
                                  std::uint32_t bitrate) {
  std::scoped_lock lock(mutex_);
  if (device == nullptr || width == 0 || height == 0 || fps == 0 ||
      (width % 2U) != 0 || (height % 2U) != 0) {
    return E_INVALIDARG;
  }
  Shutdown();
  width_ = width;
  height_ = height;
  fps_ = fps;
  bitrate_ = bitrate;
  device_ = device;
  device_->GetImmediateContext(&context_);
  RETURN_IF_FAILED(device_.As(&video_device_));
  RETURN_IF_FAILED(context_.As(&video_context_));
  RETURN_IF_FAILED(CreateVideoProcessor());
  RETURN_IF_FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  mf_started_ = true;
  RETURN_IF_FAILED(MFCreateDXGIDeviceManager(&dxgi_token_, &device_manager_));
  RETURN_IF_FAILED(device_manager_->ResetDevice(device_.Get(), dxgi_token_));

  HRESULT result = CreateEncoder(true);
  if (FAILED(result)) {
    encoder_.Reset();
    result = CreateEncoder(false);
  }
  return result;
}

HRESULT MfH264Encoder::CreateVideoProcessor() {
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputWidth = width_;
  content.InputHeight = height_;
  content.OutputWidth = width_;
  content.OutputHeight = height_;
  content.InputFrameRate = {fps_, 1};
  content.OutputFrameRate = {fps_, 1};
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  RETURN_IF_FAILED(video_device_->CreateVideoProcessorEnumerator(&content, &processor_enumerator_));
  RETURN_IF_FAILED(video_device_->CreateVideoProcessor(processor_enumerator_.Get(), 0, &processor_));

  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = width_;
  texture.Height = height_;
  texture.MipLevels = 1;
  texture.ArraySize = 1;
  texture.Format = DXGI_FORMAT_NV12;
  texture.SampleDesc.Count = 1;
  texture.Usage = D3D11_USAGE_DEFAULT;
  texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  return device_->CreateTexture2D(&texture, nullptr, &nv12_texture_);
}

HRESULT MfH264Encoder::CreateEncoder(bool hardware) {
  const MFT_REGISTER_TYPE_INFO inputInfo{MFMediaType_Video, MFVideoFormat_NV12};
  const MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activations = nullptr;
  UINT32 count = 0;
  const UINT32 flags = hardware
                           ? MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER
                           : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
  HRESULT result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inputInfo, &outputInfo,
                             &activations, &count);
  if (SUCCEEDED(result) && count == 0) {
    result = MF_E_TOPO_CODEC_NOT_FOUND;
  }
  if (SUCCEEDED(result)) {
    result = activations[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
  }
  for (UINT32 index = 0; index < count; ++index) {
    activations[index]->Release();
  }
  CoTaskMemFree(activations);
  RETURN_IF_FAILED(result);
  using_hardware_ = hardware;
  asynchronous_ = hardware;
  pending_input_requests_ = 0;
  pending_outputs_ = 0;
  event_generator_.Reset();
  if (asynchronous_) {
    RETURN_IF_FAILED(encoder_.As(&event_generator_));
  }
  return ConfigureEncoder();
}

HRESULT MfH264Encoder::ConfigureEncoder() {
  ComPtr<IMFAttributes> attributes;
  if (SUCCEEDED(encoder_->GetAttributes(&attributes))) {
    if (asynchronous_) attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
  }
  encoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                           reinterpret_cast<ULONG_PTR>(device_manager_.Get()));

  ComPtr<ICodecAPI> codecApi;
  if (SUCCEEDED(encoder_.As(&codecApi))) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_TRUE;
    SetCodecValue(codecApi.Get(), CODECAPI_AVLowLatencyMode, value);
    value.vt = VT_UI4;
    value.ulVal = eAVEncCommonRateControlMode_CBR;
    SetCodecValue(codecApi.Get(), CODECAPI_AVEncCommonRateControlMode, value);
    value.ulVal = bitrate_;
    SetCodecValue(codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, value);
    value.ulVal = fps_;
    SetCodecValue(codecApi.Get(), CODECAPI_AVEncMPVGOPSize, value);
    value.ulVal = 0;
    SetCodecValue(codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, value);
    VariantClear(&value);
  }

  ComPtr<IMFMediaType> outputType;
  RETURN_IF_FAILED(MFCreateMediaType(&outputType));
  RETURN_IF_FAILED(SetMediaTypeCommon(outputType.Get(), MFVideoFormat_H264, width_, height_, fps_));
  RETURN_IF_FAILED(outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate_));
  RETURN_IF_FAILED(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main));
  RETURN_IF_FAILED(encoder_->SetOutputType(0, outputType.Get(), 0));

  ComPtr<IMFMediaType> inputType;
  RETURN_IF_FAILED(MFCreateMediaType(&inputType));
  RETURN_IF_FAILED(SetMediaTypeCommon(inputType.Get(), MFVideoFormat_NV12, width_, height_, fps_));
  RETURN_IF_FAILED(encoder_->SetInputType(0, inputType.Get(), 0));
  RETURN_IF_FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
  RETURN_IF_FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
  return S_OK;
}

HRESULT MfH264Encoder::ConvertToNv12(ID3D11Texture2D* source) {
  D3D11_TEXTURE2D_DESC sourceDesc{};
  source->GetDesc(&sourceDesc);
  if (sourceDesc.Width != width_ || sourceDesc.Height != height_) {
    return MF_E_INVALIDMEDIATYPE;
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
  inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  inputDesc.Texture2D.ArraySlice = 0;
  inputDesc.Texture2D.MipSlice = 0;
  ComPtr<ID3D11VideoProcessorInputView> inputView;
  RETURN_IF_FAILED(video_device_->CreateVideoProcessorInputView(
      source, processor_enumerator_.Get(), &inputDesc, &inputView));

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
  outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  outputDesc.Texture2D.MipSlice = 0;
  ComPtr<ID3D11VideoProcessorOutputView> outputView;
  RETURN_IF_FAILED(video_device_->CreateVideoProcessorOutputView(
      nv12_texture_.Get(), processor_enumerator_.Get(), &outputDesc, &outputView));

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = inputView.Get();
  return video_context_->VideoProcessorBlt(processor_.Get(), outputView.Get(), 0, 1, &stream);
}

HRESULT MfH264Encoder::Encode(ID3D11Texture2D* bgraTexture, std::uint64_t timestampUs,
                              bool forceKeyframe, EncodedFrame* output) {
  std::scoped_lock lock(mutex_);
  if (encoder_ == nullptr || bgraTexture == nullptr || output == nullptr) {
    return E_INVALIDARG;
  }
  output->bytes.clear();
  output->keyframe = false;
  output->timestampUs = timestampUs;
  RETURN_IF_FAILED(ConvertToNv12(bgraTexture));

  HRESULT result = EncodeCurrentNv12(timestampUs, forceKeyframe, output);
  if (FAILED(result) && using_hardware_) {
    const HRESULT fallback = FallbackToSoftware();
    if (FAILED(fallback)) return result;
    result = EncodeCurrentNv12(timestampUs, forceKeyframe, output);
  }
  return result;
}

HRESULT MfH264Encoder::EncodeCurrentNv12(std::uint64_t timestampUs, bool forceKeyframe,
                                         EncodedFrame* output) {
  if (forceKeyframe) {
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder_.As(&codecApi))) {
      VARIANT value;
      VariantInit(&value);
      value.vt = VT_BOOL;
      value.boolVal = VARIANT_TRUE;
      SetCodecValue(codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, value);
      VariantClear(&value);
    }
  }

  ComPtr<IMFMediaBuffer> buffer;
  RETURN_IF_FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_texture_.Get(), 0,
                                             FALSE, &buffer));
  ComPtr<IMFSample> sample;
  RETURN_IF_FAILED(MFCreateSample(&sample));
  RETURN_IF_FAILED(sample->AddBuffer(buffer.Get()));
  RETURN_IF_FAILED(sample->SetSampleTime(static_cast<LONGLONG>(timestampUs * 10ULL)));
  RETURN_IF_FAILED(sample->SetSampleDuration(10'000'000LL / fps_));
  if (asynchronous_) {
    RETURN_IF_FAILED(WaitForAsyncEvent(METransformNeedInput, 250));
  }
  RETURN_IF_FAILED(encoder_->ProcessInput(0, sample.Get(), 0));
  if (asynchronous_) {
    RETURN_IF_FAILED(WaitForAsyncEvent(METransformHaveOutput, 250));
  }
  return DrainOutput(timestampUs, output);
}

HRESULT MfH264Encoder::WaitForAsyncEvent(MediaEventType expected, DWORD timeoutMs) {
  if (!asynchronous_ || event_generator_ == nullptr) return E_UNEXPECTED;
  auto consumeCredit = [&]() -> bool {
    auto* credit = expected == METransformNeedInput ? &pending_input_requests_ : &pending_outputs_;
    if (*credit == 0) return false;
    --*credit;
    return true;
  };
  if (consumeCredit()) return S_OK;

  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  while (GetTickCount64() < deadline) {
    ComPtr<IMFMediaEvent> event;
    const HRESULT getResult = event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (getResult == MF_E_NO_EVENTS_AVAILABLE) {
      Sleep(1);
      continue;
    }
    RETURN_IF_FAILED(getResult);
    HRESULT eventStatus = S_OK;
    RETURN_IF_FAILED(event->GetStatus(&eventStatus));
    RETURN_IF_FAILED(eventStatus);
    MediaEventType type = MEUnknown;
    RETURN_IF_FAILED(event->GetType(&type));
    if (type == METransformNeedInput) {
      ++pending_input_requests_;
    } else if (type == METransformHaveOutput) {
      ++pending_outputs_;
    } else if (type == MEError) {
      return E_FAIL;
    }
    if (consumeCredit()) return S_OK;
  }
  return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
}

HRESULT MfH264Encoder::FallbackToSoftware() {
  if (encoder_ != nullptr) encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  event_generator_.Reset();
  encoder_.Reset();
  asynchronous_ = false;
  using_hardware_ = false;
  pending_input_requests_ = 0;
  pending_outputs_ = 0;
  codec_config_.clear();
  return CreateEncoder(false);
}

HRESULT MfH264Encoder::DrainOutput(std::uint64_t timestampUs, EncodedFrame* output) {
  MFT_OUTPUT_STREAM_INFO streamInfo{};
  RETURN_IF_FAILED(encoder_->GetOutputStreamInfo(0, &streamInfo));
  ComPtr<IMFSample> sample;
  if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
    RETURN_IF_FAILED(MFCreateSample(&sample));
    ComPtr<IMFMediaBuffer> buffer;
    const DWORD bufferSize = std::max<DWORD>(streamInfo.cbSize, 1024U * 1024U);
    RETURN_IF_FAILED(MFCreateMemoryBuffer(bufferSize, &buffer));
    RETURN_IF_FAILED(sample->AddBuffer(buffer.Get()));
  }

  MFT_OUTPUT_DATA_BUFFER outputBuffer{};
  outputBuffer.dwStreamID = 0;
  outputBuffer.pSample = sample.Get();
  DWORD status = 0;
  const HRESULT processResult = encoder_->ProcessOutput(0, 1, &outputBuffer, &status);
  if (outputBuffer.pEvents != nullptr) {
    outputBuffer.pEvents->Release();
  }
  if (processResult == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return S_FALSE;
  }
  if (processResult == MF_E_TRANSFORM_STREAM_CHANGE) {
    RefreshCodecConfig();
    return S_FALSE;
  }
  RETURN_IF_FAILED(processResult);
  if (sample == nullptr) sample.Attach(outputBuffer.pSample);
  if (sample == nullptr) return E_UNEXPECTED;

  ComPtr<IMFMediaBuffer> contiguous;
  RETURN_IF_FAILED(sample->ConvertToContiguousBuffer(&contiguous));
  BYTE* data = nullptr;
  DWORD length = 0;
  RETURN_IF_FAILED(contiguous->Lock(&data, nullptr, &length));
  output->bytes = NormalizeAccessUnit(data, length);
  contiguous->Unlock();
  UINT32 cleanPoint = FALSE;
  output->keyframe = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) &&
                     cleanPoint != FALSE;
  RefreshCodecConfig();
  if (output->keyframe && !codec_config_.empty()) {
    output->bytes.insert(output->bytes.begin(), codec_config_.begin(), codec_config_.end());
  }
  LONGLONG sampleTime = 0;
  output->timestampUs = SUCCEEDED(sample->GetSampleTime(&sampleTime)) && sampleTime >= 0
                            ? static_cast<std::uint64_t>(sampleTime / 10)
                            : timestampUs;
  return S_OK;
}

void MfH264Encoder::RefreshCodecConfig() {
  ComPtr<IMFMediaType> type;
  if (FAILED(encoder_->GetOutputCurrentType(0, &type))) {
    return;
  }
  UINT32 size = 0;
  if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) {
    return;
  }
  std::vector<BYTE> data(size);
  UINT32 actual = 0;
  if (SUCCEEDED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, data.data(), size, &actual))) {
    codec_config_ = NormalizeCodecConfig(data.data(), actual);
  }
}

void MfH264Encoder::Shutdown() {
  if (encoder_ != nullptr) {
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  }
  encoder_.Reset();
  event_generator_.Reset();
  asynchronous_ = false;
  pending_input_requests_ = 0;
  pending_outputs_ = 0;
  codec_config_.clear();
  device_manager_.Reset();
  nv12_texture_.Reset();
  processor_.Reset();
  processor_enumerator_.Reset();
  video_context_.Reset();
  video_device_.Reset();
  context_.Reset();
  device_.Reset();
  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
}

}  // namespace hss::graphics
