cmake_minimum_required(VERSION 3.24)
project(HarmonyWebCompanionRelay LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Harmony Web Companion Relay only supports Windows 11")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)

add_library(hwc_relay_protocol STATIC
  src/relay_protocol.cpp
)
target_include_directories(hwc_relay_protocol PUBLIC src)
target_link_libraries(hwc_relay_protocol PUBLIC bcrypt crypt32)
target_compile_definitions(hwc_relay_protocol PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)
target_compile_options(hwc_relay_protocol PRIVATE /W4 /WX /permissive- /utf-8)

add_executable(harmony_web_companion_relay
  src/lan_sender.cpp
  src/main.cpp
  src/native_messaging.cpp
  src/websocket_server.cpp
)
target_include_directories(harmony_web_companion_relay PRIVATE src)
target_link_libraries(harmony_web_companion_relay PRIVATE
  hwc_relay_protocol
  bcrypt
  crypt32
  ws2_32
)
target_compile_definitions(harmony_web_companion_relay PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  UNICODE
  _UNICODE
)
target_compile_options(harmony_web_companion_relay PRIVATE
  /W4
  /WX
  /permissive-
  /utf-8
)

enable_testing()
add_executable(hwc_relay_protocol_tests
  tests/relay_protocol_tests.cpp
)
target_link_libraries(hwc_relay_protocol_tests PRIVATE hwc_relay_protocol)
target_compile_definitions(hwc_relay_protocol_tests PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)
target_compile_options(hwc_relay_protocol_tests PRIVATE
  /W4
  /WX
  /permissive-
  /utf-8
)
add_test(NAME hwc_relay_protocol_tests COMMAND hwc_relay_protocol_tests)
