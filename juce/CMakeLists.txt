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

# Polyphonic harmonic+noise engine — on by default on this branch, cross-platform
# (desktop tier: full 6-note polyphony). Distinct identity (MegaloHN) so it
# cohabits with the stock Megalo plugin in a DAW.
option(MEGALO_HN_SYNTH "Enable harmonic+noise additive synthesis" ON)
set(MEGALO_HN_QUALITY "2" CACHE STRING "Analysis tier: 0=dwarf 1=pi5 2=desktop")

juce_add_plugin(Megalo
    COMPANY_NAME "pilali"
    BUNDLE_ID com.pilali.megaloHN
    PLUGIN_MANUFACTURER_CODE Plli      # 4 chars, >=1 uppercase
    PLUGIN_CODE MgHN                   # 4 chars, >=1 uppercase — distinct from Mglo
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${COPY_AFTER_BUILD}
    FORMATS AU VST3 Standalone         # no VST2 (SDK withdrawn by Steinberg)
    VST3_CATEGORIES Fx
    AU_MAIN_TYPE kAudioUnitType_Effect
    PRODUCT_NAME "MegaloHN")

set(DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_sources(Megalo PRIVATE
    PluginProcessor.cpp PluginEditor.cpp ${DSP_DIR}/megalo_dsp.cpp)
target_include_directories(Megalo PRIVATE ${DSP_DIR})

if(MEGALO_PHASE_VOCODER)
    target_compile_definitions(Megalo PRIVATE
        MEGALO_PHASE_VOCODER MEGALO_PV_N=${MEGALO_PV_N})
endif()

if(MEGALO_HN_SYNTH)
    target_compile_definitions(Megalo PRIVATE
        MEGALO_HN_SYNTH MEGALO_HN_QUALITY=${MEGALO_HN_QUALITY})
    message(STATUS "H+N additive synthesis enabled (tier ${MEGALO_HN_QUALITY})")
endif()

target_compile_definitions(Megalo PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    # MSVC doesn't expose M_PI / M_1_PI (used in the DSP core) unless this is
    # defined before <cmath>. No-op on gcc/clang, which already provide them.
    _USE_MATH_DEFINES)

target_link_libraries(Megalo
    PRIVATE juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
