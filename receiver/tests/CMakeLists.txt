cmake_minimum_required(VERSION 3.20)
project(hss_receiver_protocol_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hss_receiver_protocol_tests
  direct_protocol_tests.cpp
  ../entry/src/main/cpp/native_protocol.cpp
  ../entry/src/main/cpp/websocket_protocol.cpp
)
target_include_directories(hss_receiver_protocol_tests PRIVATE ../entry/src/main/cpp)

enable_testing()
add_test(NAME hss_receiver_protocol_tests COMMAND hss_receiver_protocol_tests)
