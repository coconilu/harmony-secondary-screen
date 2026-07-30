#include "receiver_session.h"
#include "video_surface_layout.h"

#include <napi/native_api.h>

#include <array>
#include <cmath>
#include <string>

namespace {

using hss::receiver::ReceiverSession;

std::string ArgumentString(napi_env env, napi_value value) {
  std::size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
  std::string result(length, '\0');
  if (napi_get_value_string_utf8(env, value, result.data(), length + 1, &length) != napi_ok) return {};
  return result;
}

napi_value StartReceiver(napi_env env, napi_callback_info info) {
  std::array<napi_value, 1> arguments{};
  std::size_t count = arguments.size();
  napi_get_cb_info(env, info, &count, arguments.data(), nullptr, nullptr);
  const bool started = count == arguments.size() &&
                       ReceiverSession::Instance().Start(ArgumentString(env, arguments[0]));
  napi_value result;
  napi_get_boolean(env, started, &result);
  return result;
}

napi_value StopReceiver(napi_env env, napi_callback_info) {
  ReceiverSession::Instance().Stop();
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

napi_value ConfigureTrust(napi_env env, napi_callback_info info) {
  std::array<napi_value, 4> arguments{};
  std::size_t count = arguments.size();
  napi_get_cb_info(env, info, &count, arguments.data(), nullptr, nullptr);
  double version = 0;
  if (count == arguments.size()) {
    napi_get_value_double(env, arguments[3], &version);
  }
  const bool configured =
      count == arguments.size() &&
      ReceiverSession::Instance().ConfigureTrust(
          ArgumentString(env, arguments[0]), ArgumentString(env, arguments[1]),
          ArgumentString(env, arguments[2]), static_cast<std::uint64_t>(version));
  napi_value result;
  napi_get_boolean(env, configured, &result);
  return result;
}

napi_value AuthorizeQr(napi_env env, napi_callback_info info) {
  std::array<napi_value, 3> arguments{};
  std::size_t count = arguments.size();
  napi_get_cb_info(env, info, &count, arguments.data(), nullptr, nullptr);
  double expiresAt = 0;
  if (count == arguments.size()) {
    napi_get_value_double(env, arguments[2], &expiresAt);
  }
  const bool accepted =
      count == arguments.size() &&
      ReceiverSession::Instance().AuthorizeQr(
          ArgumentString(env, arguments[0]), ArgumentString(env, arguments[1]),
          static_cast<std::int64_t>(expiresAt));
  napi_value result;
  napi_get_boolean(env, accepted, &result);
  return result;
}

napi_value AuthorizeShortCode(napi_env env, napi_callback_info info) {
  std::array<napi_value, 1> arguments{};
  std::size_t count = arguments.size();
  napi_get_cb_info(env, info, &count, arguments.data(), nullptr, nullptr);
  const bool accepted =
      count == arguments.size() &&
      ReceiverSession::Instance().AuthorizeShortCode(ArgumentString(env, arguments[0]));
  napi_value result;
  napi_get_boolean(env, accepted, &result);
  return result;
}

napi_value ForgetDevice(napi_env env, napi_callback_info) {
  ReceiverSession::Instance().ForgetDevice();
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

void SetString(napi_env env, napi_value object, const char* key, const std::string& value) {
  napi_value text;
  napi_create_string_utf8(env, value.c_str(), value.size(), &text);
  napi_set_named_property(env, object, key, text);
}

void SetBoolean(napi_env env, napi_value object, const char* key, bool value) {
  napi_value output;
  napi_get_boolean(env, value, &output);
  napi_set_named_property(env, object, key, output);
}

void SetInteger(napi_env env, napi_value object, const char* key, std::uint64_t value) {
  napi_value output;
  napi_create_double(env, static_cast<double>(value), &output);
  napi_set_named_property(env, object, key, output);
}

napi_value GetStatus(napi_env env, napi_callback_info) {
  const auto status = ReceiverSession::Instance().Status();
  napi_value result;
  napi_create_object(env, &result);
  SetString(env, result, "state", status.state);
  SetString(env, result, "detail", status.detail);
  SetString(env, result, "listenAddress", status.listenAddress);
  SetString(env, result, "pairedAddress", status.pairedAddress);
  SetString(env, result, "deviceId", status.deviceId);
  SetBoolean(env, result, "listening", status.listening);
  SetBoolean(env, result, "connected", status.connected);
  SetBoolean(env, result, "paired", status.paired);
  SetBoolean(env, result, "automaticAddressPublished", status.automaticAddressPublished);
  SetBoolean(env, result, "automaticAddressConflict", status.automaticAddressConflict);
  SetBoolean(env, result, "automaticAddressFailed", status.automaticAddressFailed);
  SetInteger(env, result, "framesDecoded", status.framesDecoded);
  SetInteger(env, result, "framesDropped", status.framesDropped);
  SetInteger(env, result, "receivedFrames", status.receivedFrames);
  SetInteger(env, result, "mediaWidth", status.mediaWidth);
  SetInteger(env, result, "mediaHeight", status.mediaHeight);
  SetInteger(env, result, "mediaMaxFps", status.mediaMaxFps);
  return result;
}

napi_value GetPairing(napi_env env, napi_callback_info) {
  const auto pairing = ReceiverSession::Instance().Pairing();
  napi_value result;
  napi_create_object(env, &result);
  SetString(env, result, "deviceId", pairing.deviceId);
  SetString(env, result, "senderId", pairing.senderId);
  SetString(env, result, "credential", pairing.credential);
  SetInteger(env, result, "version", pairing.version);
  return result;
}

napi_value GetWifiAddresses(napi_env env, napi_callback_info) {
  const auto addresses = ReceiverSession::Instance().WifiAddresses();
  napi_value result;
  napi_create_array_with_length(env, addresses.size(), &result);
  for (std::size_t index = 0; index < addresses.size(); ++index) {
    napi_value text;
    napi_create_string_utf8(env, addresses[index].c_str(), addresses[index].size(), &text);
    napi_set_element(env, result, index, text);
  }
  return result;
}

napi_value ShouldFitVideoByWidth(napi_env env, napi_callback_info info) {
  std::array<napi_value, 4> arguments{};
  std::size_t count = arguments.size();
  napi_get_cb_info(env, info, &count, arguments.data(), nullptr, nullptr);
  std::array<double, 4> dimensions{};
  bool valid = count == arguments.size();
  for (std::size_t index = 0; valid && index < dimensions.size(); ++index) {
    valid = napi_get_value_double(env, arguments[index], &dimensions[index]) ==
            napi_ok &&
            std::isfinite(dimensions[index]) && dimensions[index] > 0;
  }
  const bool fitByWidth =
      valid && hss::receiver::ShouldFitVideoByWidth(
                   dimensions[0], dimensions[1],
                   dimensions[2], dimensions[3]);
  napi_value result;
  napi_get_boolean(env, fitByWidth, &result);
  return result;
}

napi_value OnAppForeground(napi_env env, napi_callback_info) {
  ReceiverSession::Instance().OnAppForeground();
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

napi_value OnAppBackground(napi_env env, napi_callback_info) {
  ReceiverSession::Instance().OnAppBackground();
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

void SurfaceCreated(OH_NativeXComponent* component, void* window) {
  ReceiverSession::Instance().OnSurfaceCreated(component, window);
}

void SurfaceChanged(OH_NativeXComponent* component, void* window) {
  ReceiverSession::Instance().OnSurfaceChanged(component, window);
}

void SurfaceDestroyed(OH_NativeXComponent* component, void* window) {
  ReceiverSession::Instance().OnSurfaceDestroyed(component, window);
}

napi_value Init(napi_env env, napi_value exports) {
  const std::array<napi_property_descriptor, 12> properties{{
      {"startReceiver", nullptr, StartReceiver, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stopReceiver", nullptr, StopReceiver, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"configureTrust", nullptr, ConfigureTrust, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"authorizeQr", nullptr, AuthorizeQr, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"authorizeShortCode", nullptr, AuthorizeShortCode, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"forgetDevice", nullptr, ForgetDevice, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getStatus", nullptr, GetStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getPairing", nullptr, GetPairing, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getWifiAddresses", nullptr, GetWifiAddresses, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"shouldFitVideoByWidth", nullptr, ShouldFitVideoByWidth, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"onAppForeground", nullptr, OnAppForeground, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"onAppBackground", nullptr, OnAppBackground, nullptr, nullptr, nullptr, napi_default, nullptr},
  }};
  napi_define_properties(env, exports, properties.size(), properties.data());

  napi_value xcomponentObject = nullptr;
  if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcomponentObject) == napi_ok) {
    OH_NativeXComponent* component = nullptr;
    if (napi_unwrap(env, xcomponentObject, reinterpret_cast<void**>(&component)) == napi_ok &&
        component != nullptr) {
      static OH_NativeXComponent_Callback callbacks{SurfaceCreated, SurfaceChanged,
                                                     SurfaceDestroyed, nullptr};
      OH_NativeXComponent_RegisterCallback(component, &callbacks);
    }
  }
  return exports;
}

}  // namespace

NAPI_MODULE(hss_receiver, Init)
