#include "local_security.h"

#include <sddl.h>

#include <iostream>

namespace {

bool HasAce(PACL acl, WELL_KNOWN_SID_TYPE type, DWORD requiredMask) {
  BYTE sidBuffer[SECURITY_MAX_SID_SIZE]{};
  DWORD sidSize = sizeof(sidBuffer);
  if (!CreateWellKnownSid(type, nullptr, sidBuffer, &sidSize)) return false;
  for (DWORD index = 0; index < acl->AceCount; ++index) {
    void* rawAce = nullptr;
    if (!GetAce(acl, index, &rawAce)) return false;
    const auto* header = static_cast<ACE_HEADER*>(rawAce);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
    if (EqualSid(&ace->SidStart, sidBuffer) && (ace->Mask & requiredMask) == requiredMask) {
      return true;
    }
  }
  return false;
}

bool Validate(const wchar_t* sddl, DWORD localServiceMask) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL acl = nullptr;
  const bool valid = GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                     (control & SE_DACL_PROTECTED) != 0 &&
                     GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) &&
                     present && acl != nullptr &&
                     HasAce(acl, WinLocalSystemSid, GENERIC_ALL) &&
                     HasAce(acl, WinBuiltinAdministratorsSid, GENERIC_ALL) &&
                     HasAce(acl, WinLocalServiceSid, localServiceMask) &&
                     !HasAce(acl, WinWorldSid, 0);
  LocalFree(descriptor);
  return valid;
}

}  // namespace

int main() {
  if (!Validate(hss::host::kFramesPipeSddl, GENERIC_WRITE) ||
      !Validate(hss::host::kKeyframeEventSddl, GENERIC_READ)) {
    std::cerr << "Local IPC security contract is invalid\n";
    return 1;
  }
  std::cout << "Local IPC security contract passed\n";
  return 0;
}
