#include "hss_protocol.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void VideoHeaderRoundTrip() {
  hss::protocol::VideoHeader input;
  input.session = 0x10203040U;
  input.frame = 42;
  input.fragment = 1;
  input.fragments = 3;
  input.flags = hss::protocol::VideoFlags::kKeyframe |
                hss::protocol::VideoFlags::kEndOfFrame;
  input.payloadLength = 5;
  input.timestampUs = 0x0102030405060708ULL;
  const auto header = hss::protocol::EncodeVideoHeader(input);
  std::vector<std::byte> packet(header.begin(), header.end());
  packet.resize(packet.size() + input.payloadLength);
  const auto output = hss::protocol::DecodeVideoHeader(packet);
  Require(output.has_value(), "valid video header rejected");
  Require(output->session == input.session, "session byte order mismatch");
  Require(output->frame == input.frame, "frame byte order mismatch");
  Require(output->timestampUs == input.timestampUs, "timestamp byte order mismatch");
  Require(hss::protocol::HasFlag(output->flags, hss::protocol::VideoFlags::kKeyframe),
          "keyframe flag missing");
}

void RejectMalformedVideoHeader() {
  hss::protocol::VideoHeader input;
  input.fragments = 1;
  input.payloadLength = 4;
  auto bytes = hss::protocol::EncodeVideoHeader(input);
  std::vector<std::byte> packet(bytes.begin(), bytes.end());
  Require(!hss::protocol::DecodeVideoHeader(packet).has_value(),
          "truncated video packet accepted");
  packet.resize(packet.size() + 4);
  packet[0] = std::byte{0};
  Require(!hss::protocol::DecodeVideoHeader(packet).has_value(), "bad magic accepted");
}

void ControlFramingIsIncremental() {
  const std::string json = R"({"type":"ping","timestampUs":123})";
  const auto encoded = hss::protocol::EncodeControlFrame(json);
  hss::protocol::ControlFrameDecoder decoder;
  std::vector<std::string> frames;
  std::string error;
  Require(decoder.Push(std::span(encoded).first(2), &frames, &error), "prefix rejected");
  Require(frames.empty(), "partial prefix emitted a frame");
  Require(decoder.Push(std::span(encoded).subspan(2), &frames, &error), "payload rejected");
  Require(frames.size() == 1 && frames.front() == json, "control frame mismatch");
}

void JsonFieldsAreBounded() {
  const std::string json = R"({"type":"pointer","action":"move","x":0.42,"y":0.68,"pointerId":0})";
  Require(hss::protocol::JsonString(json, "type") == "pointer", "string field parse failed");
  Require(hss::protocol::JsonInteger(json, "pointerId") == 0, "integer field parse failed");
  const auto x = hss::protocol::JsonNumber(json, "x");
  Require(x.has_value() && *x > 0.419 && *x < 0.421, "number field parse failed");
  Require(!hss::protocol::JsonString(json, "missing").has_value(), "missing key invented");
}

}  // namespace

int main() {
  VideoHeaderRoundTrip();
  RejectMalformedVideoHeader();
  ControlFramingIsIncremental();
  JsonFieldsAreBounded();
  std::cout << "protocol tests passed\n";
  return 0;
}
