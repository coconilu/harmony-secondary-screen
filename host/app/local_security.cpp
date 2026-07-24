#include "local_security.h"

#include <sddl.h>

namespace hss::host {

LocalSecurityAttributes::LocalSecurityAttributes(const wchar_t* sddl) {
  if (sddl == nullptr ||
      !ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl, SDDL_REVISION_1, &descriptor_, nullptr)) {
    descriptor_ = nullptr;
    return;
  }
  attributes_.nLength = sizeof(attributes_);
  attributes_.lpSecurityDescriptor = descriptor_;
  attributes_.bInheritHandle = FALSE;
}

LocalSecurityAttributes::~LocalSecurityAttributes() {
  if (descriptor_ != nullptr) LocalFree(descriptor_);
}

}  // namespace hss::host
