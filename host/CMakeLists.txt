cmake_minimum_required(VERSION 3.24)
project(HarmonySecondaryScreenHost LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Harmony Secondary Screen Host only supports Windows 11")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)

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
  app/local_security.cpp
  app/network_gate.cpp
  app/pointer_relay.cpp
)
target_include_directories(hss_host PRIVATE app common)
target_link_libraries(hss_host PRIVATE
  hss_protocol
  bcrypt
  advapi32
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

add_executable(hss_security_contract_tests
  tests/security_contract_tests.cpp
  app/local_security.cpp
)
target_include_directories(hss_security_contract_tests PRIVATE app)
target_link_libraries(hss_security_contract_tests PRIVATE advapi32)
target_compile_definitions(hss_security_contract_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hss_security_contract_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_security_contract_tests COMMAND hss_security_contract_tests)

add_executable(hss_network_gate_tests
  tests/network_gate_tests.cpp
  app/network_gate.cpp
)
target_include_directories(hss_network_gate_tests PRIVATE app)
target_link_libraries(hss_network_gate_tests PRIVATE iphlpapi ole32 runtimeobject ws2_32)
target_compile_definitions(hss_network_gate_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hss_network_gate_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_network_gate_tests COMMAND hss_network_gate_tests)

add_executable(hss_pointer_mapping_tests
  tests/pointer_mapping_tests.cpp
  app/pointer_injector.cpp
)
target_include_directories(hss_pointer_mapping_tests PRIVATE app common)
target_link_libraries(hss_pointer_mapping_tests PRIVATE hss_protocol user32)
target_compile_definitions(hss_pointer_mapping_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hss_pointer_mapping_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_pointer_mapping_tests COMMAND hss_pointer_mapping_tests)

add_executable(hss_receiver_connect_policy_tests tests/receiver_connect_policy_tests.cpp)
target_include_directories(hss_receiver_connect_policy_tests PRIVATE
  ../receiver/entry/src/main/cpp
)
target_compile_options(hss_receiver_connect_policy_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_receiver_connect_policy_tests COMMAND hss_receiver_connect_policy_tests)

add_executable(hss_decoder_state_tests tests/decoder_state_tests.cpp)
target_include_directories(hss_decoder_state_tests PRIVATE
  ../receiver/entry/src/main/cpp
)
target_compile_options(hss_decoder_state_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_decoder_state_tests COMMAND hss_decoder_state_tests)

add_executable(hss_async_mft_schedule_tests tests/async_mft_schedule_tests.cpp)
target_include_directories(hss_async_mft_schedule_tests PRIVATE graphics)
target_compile_options(hss_async_mft_schedule_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_async_mft_schedule_tests COMMAND hss_async_mft_schedule_tests)

add_executable(hss_data_plane_gate_tests tests/data_plane_gate_tests.cpp)
target_include_directories(hss_data_plane_gate_tests PRIVATE app)
target_compile_options(hss_data_plane_gate_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME hss_data_plane_gate_tests COMMAND hss_data_plane_gate_tests)

add_executable(hss_encoder_smoke_tests tests/encoder_smoke_tests.cpp)
target_include_directories(hss_encoder_smoke_tests PRIVATE graphics)
target_link_libraries(hss_encoder_smoke_tests PRIVATE hss_graphics)
target_compile_definitions(hss_encoder_smoke_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hss_encoder_smoke_tests PRIVATE /W4 /WX /permissive- /utf-8)
set(HSS_ENCODER_SMOKE_OUTPUT "${CMAKE_BINARY_DIR}/hss-encoder-smoke.h264")
add_test(NAME hss_encoder_smoke_tests
  COMMAND hss_encoder_smoke_tests "${HSS_ENCODER_SMOKE_OUTPUT}")
set_tests_properties(hss_encoder_smoke_tests PROPERTIES TIMEOUT 20)

find_program(HSS_FFMPEG_EXECUTABLE ffmpeg)
if(HSS_FFMPEG_EXECUTABLE)
  add_test(NAME hss_encoder_ffmpeg_decode
    COMMAND "${HSS_FFMPEG_EXECUTABLE}" -v error -y -f h264
            -i "${HSS_ENCODER_SMOKE_OUTPUT}" -frames:v 1 -f null NUL)
  set_tests_properties(hss_encoder_ffmpeg_decode PROPERTIES
    DEPENDS hss_encoder_smoke_tests
    TIMEOUT 20
  )
endif()
