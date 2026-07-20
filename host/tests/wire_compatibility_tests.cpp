#include "hss_protocol.h"
#include "native_protocol.h"

#include <cstdlib>
#include <iostream>
#include <span>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  hss::protocol::VideoHeader hostHeader;
  hostHeader.session = 0xfedcba98U;
  hostHeader.frame = 65537;
  hostHeader.fragment = 2;
  hostHeader.fragments = 4;
  hostHeader.flags = hss::protocol::VideoFlags::kKeyframe |
                     hss::protocol::VideoFlags::kEndOfFrame;
  hostHeader.payloadLength = 3;
  hostHeader.timestampUs = 9'876'543'210ULL;
  const auto bytes = hss::protocol::EncodeVideoHeader(hostHeader);
  std::vector<std::byte> datagram(bytes.begin(), bytes.end());
  datagram.resize(datagram.size() + hostHeader.payloadLength);
  const auto receiverHeader = hss::receiver::protocol::DecodeVideoHeader(datagram.data(), datagram.size());
  Require(receiverHeader.has_value(), "receiver rejected host UDP header");
  Require(receiverHeader->session == hostHeader.session, "session field differs across endpoints");
  Require(receiverHeader->frame == hostHeader.frame, "frame field differs across endpoints");
  Require(receiverHeader->timestampUs == hostHeader.timestampUs, "timestamp differs across endpoints");

  const std::string json = R"({"type":"session","sessionShort":4275878552})";
  const auto control = hss::protocol::EncodeControlFrame(json);
  hss::receiver::protocol::ControlDecoder decoder;
  std::vector<std::string> frames;
  Require(decoder.Push(control.data(), 1, &frames), "receiver rejected first control byte");
  Require(decoder.Push(control.data() + 1, control.size() - 1, &frames),
          "receiver rejected remaining control bytes");
  Require(frames.size() == 1 && frames[0] == json, "control framing differs across endpoints");
  std::cout << "wire compatibility tests passed\n";
  return 0;
}
