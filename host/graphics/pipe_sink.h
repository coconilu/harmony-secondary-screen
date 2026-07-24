#pragma once

#include "mf_h264_encoder.h"

#include <windows.h>

#include <cstdint>

namespace hss::graphics {

class PipeSink final {
 public:
  PipeSink() = default;
  ~PipeSink();
  PipeSink(const PipeSink&) = delete;
  PipeSink& operator=(const PipeSink&) = delete;

  bool Write(std::uint32_t frameNumber, const EncodedFrame& frame);
  void Close();

 private:
  bool EnsureConnected();
  bool WriteAll(const void* data, std::uint32_t size);

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}  // namespace hss::graphics
