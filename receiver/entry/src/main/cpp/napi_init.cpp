#include "receiver_session.h"

#include <napi/native_api.h>

#include <array>
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

napi_value Connect(napi_env env, napi_callback_info info) {
  std::array<napi_value, 2> arguments{};
  std::size_t count = arguments.size();
  napi_get_cb_info(env, info, &count, arguments.data(), nullptr, nullptr);
  const bool started = count == arguments.size() &&
                       ReceiverSession::Instance().Start(ArgumentString(env, arguments[0]),
                                                         ArgumentString(env, arguments[1]));
  napi_value result;
  napi_get_boolean(env, started, &result);
  return result;
}

napi_value Disconnect(napi_env env, napi_callback_info) {
  ReceiverSession::Instance().Stop();
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
  SetBoolean(env, result, "connected", status.connected);
  SetInteger(env, result, "framesDecoded", status.framesDecoded);
  SetInteger(env, result, "framesDropped", status.framesDropped);
  return result;
}

napi_value SetInputMode(napi_env env, napi_callback_info info) {
  napi_value argument;
  std::size_t count = 1;
  napi_get_cb_info(env, info, &count, &argument, nullptr, nullptr);
  if (count == 1) ReceiverSession::Instance().SetInputMode(ArgumentString(env, argument));
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

void SurfaceDestroyed(OH_NativeXComponent*, void*) {
  ReceiverSession::Instance().OnSurfaceDestroyed();
}

void Touch(OH_NativeXComponent* component, void* window) {
  ReceiverSession::Instance().OnTouch(component, window);
}

napi_value Init(napi_env env, napi_value exports) {
  const std::array<napi_property_descriptor, 4> properties{{
      {"connect", nullptr, Connect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"disconnect", nullptr, Disconnect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getStatus", nullptr, GetStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setInputMode", nullptr, SetInputMode, nullptr, nullptr, nullptr, napi_default, nullptr},
  }};
  napi_define_properties(env, exports, properties.size(), properties.data());

  napi_value xcomponentObject = nullptr;
  if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcomponentObject) == napi_ok) {
    OH_NativeXComponent* component = nullptr;
    if (napi_unwrap(env, xcomponentObject, reinterpret_cast<void**>(&component)) == napi_ok &&
        component != nullptr) {
      static OH_NativeXComponent_Callback callbacks{SurfaceCreated, SurfaceChanged,
                                                     SurfaceDestroyed, Touch};
      OH_NativeXComponent_RegisterCallback(component, &callbacks);
    }
  }
  return exports;
}

}  // namespace

NAPI_MODULE(hss_receiver, Init)
