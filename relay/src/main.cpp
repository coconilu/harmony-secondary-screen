#include "native_messaging.h"
#include "relay_protocol.h"
#include "websocket_server.h"

#include <windows.h>

#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::string WideToUtf8(const std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (size <= 0) {
    throw std::runtime_error("failed to size UTF-8 origin");
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          size,
          nullptr,
          nullptr) != size) {
    throw std::runtime_error("failed to convert UTF-8 origin");
  }
  return result;
}

[[nodiscard]] bool IsShutdownMessage(const std::string_view message) {
  return message == "{\"type\":\"shutdown\"}";
}

[[nodiscard]] bool ConfigureReceiver(
    hwc::relay::WebSocketServer* const server,
    const std::string_view message) {
  if (server == nullptr ||
      hwc::relay::JsonString(message, "type") != "configure_receiver") {
    return false;
  }
  const auto address =
      hwc::relay::JsonString(message, "receiverAddress");
  const auto pairing_code =
      hwc::relay::JsonString(message, "pairingCode");
  std::string error_code;
  if (!address || !pairing_code ||
      !server->ConfigureReceiver(
          *address,
          *pairing_code,
          &error_code)) {
    if (error_code.empty()) {
      error_code = "receiver_configuration_invalid";
    }
    hwc::relay::WriteNativeMessage(
        "{\"type\":\"error\",\"code\":\"" +
        hwc::relay::EscapeJson(error_code) + "\"}");
    return true;
  }
  hwc::relay::WriteNativeMessage(
      R"({"type":"receiver_ready","protocol":2})");
  return true;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  try {
    hwc::relay::ConfigureNativeMessagingStdio();
    if (argc < 2) {
      throw std::runtime_error("native messaging origin argument is missing");
    }

    const std::string expected_origin =
        hwc::relay::NormalizeExtensionOrigin(WideToUtf8(argv[1]));
    if (expected_origin.empty()) {
      throw std::runtime_error("native messaging origin is not an extension");
    }

    hwc::relay::WebSocketServer server(expected_origin);
    server.Start();

    std::ostringstream ready;
    ready << "{\"type\":\"ready\",\"protocol\":1,\"port\":"
          << server.port() << ",\"token\":\"" << server.token() << "\"}";
    hwc::relay::WriteNativeMessage(ready.str());

    while (true) {
      const std::optional<std::string> message =
          hwc::relay::ReadNativeMessage();
      if (!message.has_value() || IsShutdownMessage(*message)) {
        break;
      }
      if (!ConfigureReceiver(&server, *message)) {
        hwc::relay::WriteNativeMessage(
            R"({"type":"error","code":"native_message_rejected"})");
      }
    }
    server.Stop();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Harmony Web Companion Relay failed: "
              << error.what() << '\n';
    return 1;
  }
}
