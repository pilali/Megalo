cmake_minimum_required(VERSION 3.10)
project(megalo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LV2 REQUIRED lv2)

set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/megalo.lv2")

add_library(megalo MODULE src/plugin.cpp)

target_include_directories(megalo PRIVATE ${LV2_INCLUDE_DIRS} src)

target_compile_options(megalo PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -ffast-math
    -fvisibility=hidden
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-O0 -g>
)

set_target_properties(megalo PROPERTIES
    PREFIX ""
    OUTPUT_NAME "megalo"
    LIBRARY_OUTPUT_DIRECTORY "${BUNDLE_DIR}"
)

# Copy TTL files into the build bundle
add_custom_command(TARGET megalo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/megalo.lv2/manifest.ttl"
        "${CMAKE_SOURCE_DIR}/megalo.lv2/megalo.ttl"
        "${BUNDLE_DIR}/"
    COMMENT "Copying TTL files to bundle"
)

install(DIRECTORY "${BUNDLE_DIR}"
    DESTINATION lib/lv2
)
