add_executable(input_probe input_probe.cpp)

set(LOGI_SDK_ROOT "${CMAKE_SOURCE_DIR}/vendor/LogitechSDK")
target_include_directories(input_probe PRIVATE "${LOGI_SDK_ROOT}/Include")
target_link_libraries(input_probe PRIVATE "${LOGI_SDK_ROOT}/Lib/x64/LogitechSteeringWheelLib.lib")

set_target_properties(input_probe PROPERTIES OUTPUT_NAME "input_probe")
