#include "annex_b.h"
#include "com_apartment.h"
#include "mf_h264_encoder.h"

#include <d3d11.h>
#include <mferror.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool FindNalTypes(const std::vector<std::byte>& bytes, bool* sps, bool* pps, bool* idr) {
  for (std::size_t index = 0; index + 4 < bytes.size();) {
    std::size_t prefix = 0;
    if (bytes[index] == std::byte{0} && bytes[index + 1] == std::byte{0} &&
        bytes[index + 2] == std::byte{1}) {
      prefix = 3;
    } else if (index + 4 < bytes.size() && bytes[index] == std::byte{0} &&
               bytes[index + 1] == std::byte{0} && bytes[index + 2] == std::byte{0} &&
               bytes[index + 3] == std::byte{1}) {
      prefix = 4;
    }
    if (prefix == 0) {
      ++index;
      continue;
    }
    const auto type = std::to_integer<std::uint8_t>(bytes[index + prefix]) & 0x1fU;
    *sps = *sps || type == 7;
    *pps = *pps || type == 8;
    *idr = *idr || type == 5;
    index += prefix + 1;
  }
  return *sps && *pps && *idr;
}

}  // namespace

int RunEncoderSmoke(const char* outputPath) {
  constexpr UINT width = 320;
  constexpr UINT height = 240;
  constexpr UINT fps = 30;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL featureLevel{};
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                     D3D11_SDK_VERSION, &device, &featureLevel, &context);
  if (FAILED(result)) {
    result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                               D3D11_SDK_VERSION, &device, &featureLevel, &context);
  }
  if (FAILED(result)) {
    std::cerr << "D3D11 video device unavailable: 0x" << std::hex << result << '\n';
    return 1;
  }

  std::vector<std::uint32_t> pixels(width * height, 0xff204080U);
  D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * sizeof(std::uint32_t), 0};
  D3D11_TEXTURE2D_DESC textureDesc{};
  textureDesc.Width = width;
  textureDesc.Height = height;
  textureDesc.MipLevels = 1;
  textureDesc.ArraySize = 1;
  textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Usage = D3D11_USAGE_DEFAULT;
  textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> texture;
  result = device->CreateTexture2D(&textureDesc, &initial, &texture);
  if (FAILED(result)) return 1;

  hss::graphics::MfH264Encoder encoder;
  result = encoder.Initialize(device.Get(), width, height, fps, 1'000'000);
  if (FAILED(result)) {
    std::cerr << "H.264 encoder unavailable: 0x" << std::hex << result << '\n';
    return 1;
  }

  std::vector<std::byte> stream;
  bool hasSps = false;
  bool hasPps = false;
  bool hasIdr = false;
  bool sawCodecConfigMetadata = false;
  for (std::uint64_t frame = 0; frame < 90 && !(hasSps && hasPps && hasIdr); ++frame) {
    hss::graphics::EncodedFrame output;
    result = encoder.Encode(texture.Get(), frame * (1'000'000ULL / fps), frame == 0, &output);
    if (FAILED(result)) {
      std::cerr << "Encode failed: 0x" << std::hex << result << '\n';
      return 1;
    }
    if (!output.bytes.empty()) {
      bool outputSps = false;
      bool outputPps = false;
      bool outputIdr = false;
      FindNalTypes(output.bytes, &outputSps, &outputPps, &outputIdr);
      if (output.hasCodecConfig != (outputSps && outputPps)) {
        std::cerr << "Codec-config metadata did not match Annex-B SPS/PPS\n";
        return 1;
      }
      sawCodecConfigMetadata = sawCodecConfigMetadata || output.hasCodecConfig;
      hasSps = hasSps || outputSps;
      hasPps = hasPps || outputPps;
      hasIdr = hasIdr || outputIdr;
      stream.insert(stream.end(), output.bytes.begin(), output.bytes.end());
    }
  }
  if (!hasSps || !hasPps || !hasIdr || !sawCodecConfigMetadata || stream.empty()) {
    std::cerr << "Encoded stream lacks Annex-B SPS/PPS/IDR\n";
    return 1;
  }
  std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(stream.data()),
             static_cast<std::streamsize>(stream.size()));
  if (!file) return 1;
  std::cout << "Produced decodable Annex-B candidate: " << stream.size() << " bytes; "
            << (encoder.using_hardware() ? "hardware" : "software/fallback") << '\n';
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::vector<std::byte> idrWithoutSequenceHeaders{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x65}};
  if (hss::graphics::ContainsAvcCodecConfig(idrWithoutSequenceHeaders)) {
    std::cerr << "IDR without SPS/PPS produced false codec-config metadata\n";
    return 1;
  }
  int result = 1;
  // The production encoder also starts on a fresh std::thread. Do not
  // initialize COM on this main thread; prove the thread entry owns its MTA.
  std::thread encoderThread([&] {
    hss::graphics::ComMtaApartment apartment;
    APTTYPE apartmentType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    if (!apartment.ready() || FAILED(CoGetApartmentType(&apartmentType, &qualifier)) ||
        apartmentType != APTTYPE_MTA) {
      std::cerr << "Encoder worker did not establish an MTA: 0x" << std::hex
                << apartment.result() << '\n';
      result = 1;
      return;
    }
    result = RunEncoderSmoke(argv[1]);
  });
  encoderThread.join();
  return result;
}
