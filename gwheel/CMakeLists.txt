set(GWHEEL_SOURCES
  src/dllmain.cpp
  src/plugin.cpp
  src/device_table.cpp
  src/wheel.cpp
  src/sources.cpp
  src/vehicle_hook.cpp
  src/input_bindings.cpp
  src/axis_resolver.cpp
  src/kbd_hook.cpp
  src/config.cpp
  src/rtti.cpp
  src/rtti_offsets.cpp
  src/tweakdb_physics.cpp
  src/logging.cpp
  src/audio_monitor.cpp
  src/led.cpp
  src/mod_settings_seed.cpp
)

set(GWHEEL_HEADERS
  src/plugin.h
  src/logging.h
  src/device_table.h
  src/wheel.h
  src/sources.h
  src/vehicle_hook.h
  src/vehicle_physics_ext.h
  src/input_bindings.h
  src/axis_resolver.h
  src/kbd_hook.h
  src/config.h
  src/rtti.h
  src/rtti_offsets.h
  src/tweakdb_physics.h
  src/audio_monitor.h
  src/led.h
  src/mod_settings_seed.h
  include/gwheel_abi.h
)

add_library(gwheel SHARED
  ${GWHEEL_SOURCES}
  ${GWHEEL_HEADERS}
)

# Logitech Steering Wheel SDK (vendored under vendor/LogitechSDK/).
set(LOGI_SDK_ROOT "${CMAKE_SOURCE_DIR}/vendor/LogitechSDK")
set(LOGI_SDK_INCLUDE_DIR "${LOGI_SDK_ROOT}/Include")
set(LOGI_SDK_LIB "${LOGI_SDK_ROOT}/Lib/x64/LogitechSteeringWheelLib.lib")
if(NOT EXISTS "${LOGI_SDK_LIB}")
  message(FATAL_ERROR
    "Logitech SDK import lib not found at ${LOGI_SDK_LIB}. "
    "Ensure vendor/LogitechSDK/ is present.")
endif()

target_include_directories(gwheel
  PRIVATE
    src
    include
    "${LOGI_SDK_INCLUDE_DIR}"
)

target_link_libraries(gwheel
  PRIVATE
    RED4ext::SDK
    "${LOGI_SDK_LIB}"
    dinput8
    dxguid
    # WASAPI loopback capture for the LED music visualizer:
    #   ole32     — CoInitializeEx / CoCreateInstance
    #   uuid      — GUID constants for MMDevice / IAudioClient / KSDATAFORMAT_*
    #   mmdevapi  — ActivateAudioInterfaceAsync (per-process loopback API)
    ole32
    uuid
    mmdevapi
)

set_target_properties(gwheel PROPERTIES
  OUTPUT_NAME "gwheel"
  PREFIX ""
  SUFFIX ".dll"
)
