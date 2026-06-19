cmake_minimum_required(VERSION 3.22)

# macOS: universal binaries (arm64 + x86_64) + min OS. MUST be set before project().
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS archs")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "min macOS")
endif()

project(Megalo VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: fetched automatically. Override -DLOCAL_JUCE_DIR=/path for offline builds.
set(JUCE_TAG "8.0.8" CACHE STRING "JUCE git tag")
option(COPY_AFTER_BUILD "Install into user plugin folders after build" ON)
if(DEFINED LOCAL_JUCE_DIR)
    add_subdirectory(${LOCAL_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${JUCE_TAG} GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# Phase-vocoder pitch shifter. Kept ON so BOTH pitch engines are compiled into
# the plugin and the "Pitch Engine" parameter selects between them at runtime
# (granular by default — matches the LV2 MOD/native sound). FFT size 2048 like
# the Pi 5 build. (Turning this OFF builds a granular-only, lower-memory binary.)
option(MEGALO_PHASE_VOCODER "Compile in phase vocoder pitch engine" ON)
set(MEGALO_PV_N "2048" CACHE STRING "Phase vocoder FFT size (power of 2)")

# Polyphonic harmonic+noise engine tier (cross-platform desktop tier: full
# 6-note polyphony). The flag itself is applied per-target below: this build
# produces TWO distinct plugins from the shared DSP core — the stock granular
# Megalo and the polyphonic MegaloHN — so they cohabit in a DAW.
set(MEGALO_HN_QUALITY "2" CACHE STRING "Analysis tier: 0=dwarf 1=pi5 2=desktop")

# Build switches for the two plugins (both default ON).
option(MEGALO_BUILD_STD "Build the stock granular Megalo plugin" ON)
option(MEGALO_BUILD_HN  "Build the polyphonic harmonic+noise MegaloHN plugin" ON)

set(DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Shared per-plugin setup: sources, includes, the optional phase-vocoder
# defines, the common JUCE config defines, and the link libraries.
#   _target   CMake target name (created by juce_add_plugin beforehand)
function(megalo_configure_juce _target)
    target_sources(${_target} PRIVATE
        PluginProcessor.cpp PluginEditor.cpp ${DSP_DIR}/megalo_dsp.cpp)
    target_include_directories(${_target} PRIVATE ${DSP_DIR})

    if(MEGALO_PHASE_VOCODER)
        target_compile_definitions(${_target} PRIVATE
            MEGALO_PHASE_VOCODER MEGALO_PV_N=${MEGALO_PV_N})
    endif()

    target_compile_definitions(${_target} PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0
        # MSVC doesn't expose M_PI / M_1_PI (used in the DSP core) unless this
        # is defined before <cmath>. No-op on gcc/clang.
        _USE_MATH_DEFINES)

    target_link_libraries(${_target}
        PRIVATE juce::juce_audio_utils
        PUBLIC  juce::juce_recommended_config_flags
                juce::juce_recommended_lto_flags
                juce::juce_recommended_warning_flags)
endfunction()

# ── Stock granular Megalo (MEGALO_HN_SYNTH off) ────────────────────────────
if(MEGALO_BUILD_STD)
    juce_add_plugin(Megalo
        COMPANY_NAME "pilali"
        BUNDLE_ID com.pilali.megalo
        PLUGIN_MANUFACTURER_CODE Plli  # 4 chars, >=1 uppercase
        PLUGIN_CODE Mglo               # 4 chars, >=1 uppercase
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD ${COPY_AFTER_BUILD}
        FORMATS AU VST3 Standalone     # no VST2 (SDK withdrawn by Steinberg)
        VST3_CATEGORIES Fx
        AU_MAIN_TYPE kAudioUnitType_Effect
        PRODUCT_NAME "Megalo")
    megalo_configure_juce(Megalo)
    message(STATUS "Megalo (granular) JUCE plugin enabled")
endif()

# ── Polyphonic MegaloHN (MEGALO_HN_SYNTH on) ───────────────────────────────
if(MEGALO_BUILD_HN)
    juce_add_plugin(MegaloHN
        COMPANY_NAME "pilali"
        BUNDLE_ID com.pilali.megaloHN
        PLUGIN_MANUFACTURER_CODE Plli  # 4 chars, >=1 uppercase
        PLUGIN_CODE MgHN               # 4 chars, >=1 uppercase — distinct from Mglo
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD ${COPY_AFTER_BUILD}
        FORMATS AU VST3 Standalone
        VST3_CATEGORIES Fx
        AU_MAIN_TYPE kAudioUnitType_Effect
        PRODUCT_NAME "MegaloHN")
    megalo_configure_juce(MegaloHN)
    target_compile_definitions(MegaloHN PRIVATE
        MEGALO_HN_SYNTH MEGALO_HN_QUALITY=${MEGALO_HN_QUALITY})
    message(STATUS "MegaloHN (H+N additive, tier ${MEGALO_HN_QUALITY}) JUCE plugin enabled")
endif()
