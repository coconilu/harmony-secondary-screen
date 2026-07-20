#include "connect_policy.h"

#include <iostream>

using hss::receiver::ConnectWaitDecision;
using hss::receiver::EvaluateConnectWait;

int main() {
  if (EvaluateConnectWait(false, false, false, 0) != ConnectWaitDecision::kCancelled ||
      EvaluateConnectWait(true, true, false, 0) != ConnectWaitDecision::kTimedOut ||
      EvaluateConnectWait(true, false, false, 0) != ConnectWaitDecision::kContinue ||
      EvaluateConnectWait(true, false, true, 0) != ConnectWaitDecision::kConnected ||
      EvaluateConnectWait(true, false, true, 10061) != ConnectWaitDecision::kFailed) {
    std::cerr << "Receiver connect cancellation/deadline policy failed\n";
    return 1;
  }
  std::cout << "Receiver connect cancellation/deadline policy passed\n";
  return 0;
}
