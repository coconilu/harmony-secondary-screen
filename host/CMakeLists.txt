cmake_minimum_required(VERSION 3.24)
project(HarmonySecondaryScreenHost LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Harmony Secondary Screen Host only supports Windows 11")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hss_protocol STATIC
  common/hss_protocol.cpp
)
target_include_directories(hss_protocol PUBLIC common)
target_compile_definitions(hss_protocol PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hss_protocol PRIVATE /W4 /WX /permissive- /utf-8)

add_library(hss_graphics STATIC
  graphics/mf_h264_encoder.cpp
  graphics/pipe_sink.cpp
)
target_include_directories(hss_graphics PUBLIC graphics)
target_link_libraries(hss_graphics PUBLIC
  d3d11
  dxgi
  mf
  mfplat
  mfuuid
  ole32
)
target_compile_definitions(hss_graphics PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(hss_graphics PRIVATE /W4 /WX /permissive- /utf-8)

add_executable(hss_host
  app/main.cpp
  app/host_server.cpp
  app/network_gate.cpp
  app/pointer_relay.cpp
)
target_include_directories(hss_host PRIVATE app common)
target_link_libraries(hss_host PRIVATE
  hss_protocol
  bcrypt
  iphlpapi
  ole32
  runtimeobject
  ws2_32
)
target_compile_definitions(hss_host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(hss_host PRIVATE /W4 /WX /permissive- /utf-8)

add_executable(hss_input_agent
  app/input_agent.cpp
  app/pointer_injector.cpp
)
target_include_directories(hss_input_agent PRIVATE app common)
target_link_libraries(hss_input_agent PRIVATE hss_protocol)
target_compile_definitions(hss_input_agent PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(hss_input_agent PRIVATE /W4 /WX /permissive- /utf-8)

enable_testing()
add_executable(hss_protocol_tests tests/protocol_tests.cpp)
target_link_libraries(hss_protocol_tests PRIVATE hss_protocol ws2_32)
target_compile_definitions(hss_protocol_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hss_protocol_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_protocol_tests COMMAND hss_protocol_tests)

add_executable(hss_wire_compatibility_tests
  tests/wire_compatibility_tests.cpp
  ../receiver/entry/src/main/cpp/native_protocol.cpp
)
target_include_directories(hss_wire_compatibility_tests PRIVATE
  common
  ../receiver/entry/src/main/cpp
)
target_link_libraries(hss_wire_compatibility_tests PRIVATE hss_protocol)
target_compile_definitions(hss_wire_compatibility_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hss_wire_compatibility_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_wire_compatibility_tests COMMAND hss_wire_compatibility_tests)
