#pragma once

namespace hss::receiver {

enum class ConnectWaitDecision { kContinue, kConnected, kFailed, kCancelled, kTimedOut };

constexpr ConnectWaitDecision EvaluateConnectWait(bool desired, bool deadlineReached,
                                                  bool socketReady, int socketError) {
  if (!desired) return ConnectWaitDecision::kCancelled;
  if (deadlineReached) return ConnectWaitDecision::kTimedOut;
  if (!socketReady) return ConnectWaitDecision::kContinue;
  return socketError == 0 ? ConnectWaitDecision::kConnected : ConnectWaitDecision::kFailed;
}

}  // namespace hss::receiver
