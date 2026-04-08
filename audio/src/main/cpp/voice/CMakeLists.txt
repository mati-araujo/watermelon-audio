# ============================================================================
# watermelon-voice: Polyphonic voice system sub-library
#
# Phase 1D — Audio Library Extraction
#
# Depends on: watermelon-engines (SynthEngine), watermelon-dsp (ParameterSmoother)
# Also needs: oscillators/Oscillators.h and core/AudioSource.h (legacy base classes)
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)

add_library(watermelon-voice STATIC
    Voice.cpp
    VoicePool.cpp
    VoiceManager.cpp
    TouchTriggerSource.cpp
)

target_include_directories(watermelon-voice
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    # Legacy: oscillators and AudioSource are not yet in a sub-library
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../oscillators
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core
)

target_link_libraries(watermelon-voice PUBLIC watermelon-engines watermelon-dsp)
target_compile_features(watermelon-voice PUBLIC cxx_std_20)
