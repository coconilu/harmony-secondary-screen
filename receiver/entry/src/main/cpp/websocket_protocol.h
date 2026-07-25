#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hss::receiver::websocket {

constexpr std::size_t kMaxMessageBytes = 8U * 1024U * 1024U + 32U;

#ifdef _WIN32
using SocketHandle = std::uintptr_t;
#else
using SocketHandle = int;
#endif

enum class Opcode : std::uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

struct Message {
  Opcode opcode = Opcode::kClose;
  std::vector<std::byte> payload;
};

bool BuildUpgradeResponse(std::string_view request, std::string* response);
bool ReadUpgradeRequest(SocketHandle socket,
                        std::chrono::steady_clock::time_point deadline,
                        std::string* request);
std::vector<std::byte> EncodeFrame(Opcode opcode, std::string_view payload);

class Decoder final {
 public:
  bool Push(const std::byte* data, std::size_t size, std::vector<Message>* messages);
  void Reset();

 private:
  std::vector<std::byte> buffer_;
  std::vector<std::byte> fragmented_;
  std::optional<Opcode> fragmented_opcode_;
};

}  // namespace hss::receiver::websocket
