add_executable(wheel_reset wheel_reset.cpp)

set(LOGI_SDK_ROOT "${CMAKE_SOURCE_DIR}/vendor/LogitechSDK")
target_include_directories(wheel_reset PRIVATE "${LOGI_SDK_ROOT}/Include")
target_link_libraries(wheel_reset PRIVATE "${LOGI_SDK_ROOT}/Lib/x64/LogitechSteeringWheelLib.lib")

set_target_properties(wheel_reset PROPERTIES
  OUTPUT_NAME "wheel_reset"
)
