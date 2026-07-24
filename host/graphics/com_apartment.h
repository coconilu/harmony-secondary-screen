#pragma once

#include <objbase.h>

namespace hss::graphics {

class ComMtaApartment final {
 public:
  ComMtaApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComMtaApartment() {
    if (SUCCEEDED(result_)) CoUninitialize();
  }
  ComMtaApartment(const ComMtaApartment&) = delete;
  ComMtaApartment& operator=(const ComMtaApartment&) = delete;

  bool ready() const { return SUCCEEDED(result_); }
  HRESULT result() const { return result_; }

 private:
  HRESULT result_;
};

}  // namespace hss::graphics
