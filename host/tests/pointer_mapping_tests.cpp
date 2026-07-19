#include "pointer_injector.h"

#include <iostream>

int main() {
  if (!hss::host::PointerInjector::IsHarmonyAdapterPath(
          LR"(\\?\ROOT#HarmonySecondaryScreenIdd#0000#{display-guid})") ||
      !hss::host::PointerInjector::IsHarmonyAdapterPath(
          LR"(\\?\root#HARMONYSECONDARYSCREENIDD#0001)") ||
      hss::host::PointerInjector::IsHarmonyAdapterPath(
          LR"(\\?\PCI#VEN_10DE&DEV_0000#display)")) {
    std::cerr << "Stable display-adapter path matching failed\n";
    return 1;
  }
  std::cout << "Stable display-adapter path matching passed\n";
  return 0;
}
