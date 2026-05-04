cmake_minimum_required(VERSION 3.21)

project(gwheel
  VERSION 2.31.2
  DESCRIPTION "Logitech G-series steering wheel mod for Cyberpunk 2077"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "gwheel targets Windows only (Cyberpunk 2077 is Windows-native).")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
  )
endif()

add_subdirectory(vendor/RED4ext.SDK EXCLUDE_FROM_ALL)
add_subdirectory(gwheel)
add_subdirectory(tools/wheel_reset)
add_subdirectory(tools/input_probe)
add_subdirectory(tools/trueforce_probe)
add_subdirectory(tools/libtrueforce_probe)
