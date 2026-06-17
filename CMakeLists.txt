cmake_minimum_required(VERSION 3.10)
project(megalo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LV2 REQUIRED lv2)

# ── Options ───────────────────────────────────────────────────────────────
option(MEGALO_PHASE_VOCODER
    "Enable phase vocoder pitch shifter (higher quality, more CPU)" OFF)
set(MEGALO_PV_N "2048" CACHE STRING
    "Phase vocoder FFT size — must be a power of 2 (1024 for MOD Dwarf, 2048 for Pi 5)")

option(MEGALO_HN_SYNTH
    "Enable harmonic+noise additive synthesis (replaces granular/PV on pitched sources)" ON)
option(MEGALO_RAVE
    "Enable RAVE neural synthesis decoder stub (RPi 5 only — requires trained model + ONNX RT)" OFF)
set(MEGALO_HN_QUALITY "2" CACHE STRING
    "Polyphonic analysis tier: 0=dwarf (A35), 1=pi5 (A76), 2=desktop")

set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/megaloHN.lv2")

add_library(megalo MODULE src/plugin.cpp src/megalo_dsp.cpp)

target_include_directories(megalo PRIVATE ${LV2_INCLUDE_DIRS} src)

target_compile_options(megalo PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -ffast-math
    -fvisibility=hidden
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-O0 -g>
)

if(MEGALO_PHASE_VOCODER)
    target_compile_definitions(megalo PRIVATE
        MEGALO_PHASE_VOCODER
        MEGALO_PV_N=${MEGALO_PV_N}
    )
    message(STATUS "Phase vocoder enabled — FFT size: ${MEGALO_PV_N}")
else()
    message(STATUS "Phase vocoder disabled — using variable-speed reader")
endif()

if(MEGALO_HN_SYNTH)
    target_compile_definitions(megalo PRIVATE
        MEGALO_HN_SYNTH MEGALO_HN_QUALITY=${MEGALO_HN_QUALITY})
    message(STATUS "H+N additive synthesis enabled (tier ${MEGALO_HN_QUALITY})")
    if(MEGALO_RAVE)
        target_compile_definitions(megalo PRIVATE MEGALO_RAVE)
        message(STATUS "RAVE stub enabled (RPi 5 — model + ONNX RT required)")
        # TODO: find_package(ONNXRuntime REQUIRED)
        # target_link_libraries(megalo PRIVATE onnxruntime)
    endif()
endif()

set_target_properties(megalo PROPERTIES
    PREFIX ""
    OUTPUT_NAME "megaloHN"
    LIBRARY_OUTPUT_DIRECTORY "${BUNDLE_DIR}"
)

# Copy TTL files into the build bundle
add_custom_command(TARGET megalo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/manifest.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/megaloHN.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/modgui.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Choir.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Fast.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Good.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Good_2.ttl"
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/Grainy.ttl"
        "${BUNDLE_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/megaloHN.lv2/modgui" "${BUNDLE_DIR}/modgui"
    COMMENT "Copying TTL + modgui files to bundle"
)

install(DIRECTORY "${BUNDLE_DIR}"
    DESTINATION lib/lv2
)
