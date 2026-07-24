cmake_minimum_required(VERSION 3.5.0)
project(hss_receiver)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hss_receiver SHARED
  napi_init.cpp
  native_protocol.cpp
  receiver_session.cpp
)

target_compile_options(hss_receiver PRIVATE -Wall -Wextra -Werror -Wno-unused-command-line-argument)
target_link_libraries(hss_receiver PUBLIC
  libace_napi.z.so
  libace_ndk.z.so
  libhilog_ndk.z.so
  libnative_media_codecbase.so
  libnative_media_core.so
  libnative_media_vdec.so
)
