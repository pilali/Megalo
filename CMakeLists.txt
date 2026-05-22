cmake_minimum_required(VERSION 3.10)
project(megaloHN VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LV2 REQUIRED lv2)

# ── Options ───────────────────────────────────────────────────────────────
option(MEGALO_PHASE_VOCODER
    "Enable phase vocoder pitch shifter (higher quality, more CPU)" OFF)
set(MEGALO_PV_N "2048" CACHE STRING
    "Phase vocoder FFT size — must be a power of 2 (1024 for MOD Dwarf, 2048 for Pi 5)")

set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/megaloHN.lv2")

add_library(megaloHN MODULE src/plugin.cpp src/glibc_compat.cpp)

target_include_directories(megaloHN PRIVATE ${LV2_INCLUDE_DIRS} src)

target_compile_options(megaloHN PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -ffast-math
    -fvisibility=hidden
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-O0 -g>
)

if(MEGALO_PHASE_VOCODER)
    target_compile_definitions(megaloHN PRIVATE
        MEGALO_PHASE_VOCODER
        MEGALO_PV_N=${MEGALO_PV_N}
    )
    message(STATUS "Phase vocoder enabled — FFT size: ${MEGALO_PV_N}")
else()
    message(STATUS "Phase vocoder disabled — using variable-speed reader")
endif()

set_target_properties(megaloHN PROPERTIES
    PREFIX ""
    OUTPUT_NAME "megaloHN"
    LIBRARY_OUTPUT_DIRECTORY "${BUNDLE_DIR}"
)

# Copy TTL files and presets into the build bundle
add_custom_command(TARGET megaloHN POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/manifest.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/megaloHN.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Choir.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Fast.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Good.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Good_2.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Grainy.ttl"
        "${BUNDLE_DIR}/"
    COMMENT "Copying TTL files to bundle"
)

install(DIRECTORY "${BUNDLE_DIR}"
    DESTINATION lib/lv2
)
