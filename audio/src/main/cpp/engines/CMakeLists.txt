# ============================================================================
# watermelon-engines: Synth engines sub-library
#
# Phase 1C — Audio Library Extraction
#
# Depends on: watermelon-dsp (ParameterSmoother, DSPMath)
# External: tinysoundfont (for SoundFontEngine/SoundFontManager)
#
# Engines: Classic (in OscillatorNode), KarplusStrong, FM, Supersaw,
#          Wavetable, Granular, SoundFont
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)

add_library(watermelon-engines STATIC
    # Only SoundFontManager has a .cpp — rest are header-only
    SoundFontManager.cpp
)

target_include_directories(watermelon-engines PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(watermelon-engines PUBLIC watermelon-dsp)
target_link_libraries(watermelon-engines PRIVATE tinysoundfont)
target_compile_features(watermelon-engines PUBLIC cxx_std_20)
