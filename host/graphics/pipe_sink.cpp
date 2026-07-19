#include "pipe_sink.h"

#include <array>

namespace hss::graphics {
namespace {

constexpr std::uint32_t kPipeMagic = 0x48535046U; // HSPF

#pragma pack(push, 1)
struct PipeFrameHeader {
  std::uint32_t magic;
  std::uint32_t payloadSize;
  std::uint32_t frame;
  std::uint32_t flags;
  std::uint64_t timestampUs;
};
#pragma pack(pop)

static_assert(sizeof(PipeFrameHeader) == 24);

}  // namespace

PipeSink::~PipeSink() {
  Close();
}

bool PipeSink::EnsureConnected() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    return true;
  }
  pipe_ = CreateFileW(L"\\\\.\\pipe\\HarmonySecondaryScreen.Frames", GENERIC_WRITE, 0,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  return pipe_ != INVALID_HANDLE_VALUE;
}

bool PipeSink::WriteAll(const void* data, std::uint32_t size) {
  const auto* bytes = static_cast<const std::byte*>(data);
  std::uint32_t total = 0;
  while (total < size) {
    DWORD written = 0;
    if (!WriteFile(pipe_, bytes + total, size - total, &written, nullptr) || written == 0) {
      Close();
      return false;
    }
    total += written;
  }
  return true;
}

bool PipeSink::Write(std::uint32_t frameNumber, const EncodedFrame& frame, bool codecConfig) {
  if (frame.bytes.empty() || frame.bytes.size() > UINT32_MAX || !EnsureConnected()) {
    return false;
  }
  PipeFrameHeader header{kPipeMagic,
                         static_cast<std::uint32_t>(frame.bytes.size()),
                         frameNumber,
                         (frame.keyframe ? 1U : 0U) | (codecConfig ? 2U : 0U),
                         frame.timestampUs};
  return WriteAll(&header, sizeof(header)) &&
         WriteAll(frame.bytes.data(), static_cast<std::uint32_t>(frame.bytes.size()));
}

void PipeSink::Close() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
}

}  // namespace hss::graphics
