add_executable(libtrueforce_probe
    libtrueforce_probe.cpp
    ${CMAKE_SOURCE_DIR}/gwheel/src/lib_tf/tf_hid.cpp
)
target_include_directories(libtrueforce_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/gwheel/src
    ${CMAKE_SOURCE_DIR}/gwheel/src/lib_tf
)
target_link_libraries(libtrueforce_probe PRIVATE setupapi hid)
set_target_properties(libtrueforce_probe PROPERTIES OUTPUT_NAME "libtrueforce_probe")
