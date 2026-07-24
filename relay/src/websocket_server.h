#pragma once

#include "lan_sender.h"

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace hwc::relay {

class WebSocketServer final {
 public:
  explicit WebSocketServer(std::string expected_origin);
  ~WebSocketServer();

  WebSocketServer(const WebSocketServer&) = delete;
  WebSocketServer& operator=(const WebSocketServer&) = delete;

  void Start();
  void Stop();
  [[nodiscard]] bool ConfigureReceiver(
      std::string receiver_address,
      std::string pairing_code,
      std::string* error_code);

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] const std::string& token() const noexcept;

 private:
  void Run();
  void RunClient(SOCKET client);

  std::string expected_origin_;
  std::string token_;
  std::atomic<SOCKET> listener_{INVALID_SOCKET};
  std::atomic<SOCKET> client_{INVALID_SOCKET};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> client_authenticated_{false};
  std::uint16_t port_ = 0;
  std::thread worker_;
  LanSender lan_sender_;
  bool winsock_started_ = false;
};

}  // namespace hwc::relay
