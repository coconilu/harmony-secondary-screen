#include "data_plane_gate.h"

#include <chrono>
#include <future>
#include <iostream>

int main() {
  hss::host::DataPlaneGate gate;
  const auto first = gate.Open();
  if (!gate.CanSend(first)) return 1;
  gate.Revoke();

  std::size_t packetsAfterNetworkDowngrade = 0;
  for (std::size_t fragment = 0; fragment < 2048; ++fragment) {
    gate.RunIfAllowed(first, [&] { ++packetsAfterNetworkDowngrade; });
  }
  if (packetsAfterNetworkDowngrade != 0 || gate.Capture().has_value()) {
    std::cerr << "Revoked session emitted video packets\n";
    return 1;
  }
  const auto second = gate.Open();
  if (second == first || gate.CanSend(first) || !gate.CanSend(second)) {
    std::cerr << "Session epoch isolation failed\n";
    return 1;
  }

  std::promise<void> sendStarted;
  std::promise<void> releaseSend;
  auto releaseFuture = releaseSend.get_future();
  auto inFlightSend = std::async(std::launch::async, [&] {
    return gate.RunIfAllowed(second, [&] {
      sendStarted.set_value();
      releaseFuture.wait();
    });
  });
  sendStarted.get_future().wait();
  auto revoke = std::async(std::launch::async, [&] { gate.Revoke(); });
  if (revoke.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) {
    std::cerr << "Revoke did not wait for the in-flight packet lease\n";
    releaseSend.set_value();
    return 1;
  }
  releaseSend.set_value();
  if (!inFlightSend.get() ||
      revoke.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
    std::cerr << "In-flight packet lease did not drain\n";
    return 1;
  }
  revoke.get();
  if (gate.RunIfAllowed(second, [&] { ++packetsAfterNetworkDowngrade; }) ||
      packetsAfterNetworkDowngrade != 0) {
    std::cerr << "Packet crossed the completed revoke boundary\n";
    return 1;
  }
  std::cout << "Network downgrade produces zero subsequent video packets\n";
  return 0;
}
