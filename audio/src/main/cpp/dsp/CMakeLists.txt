# ============================================================================
# watermelon-dsp: Pure DSP primitives sub-library
#
# Phase 1A — Audio Library Extraction
#
# Only the C++ standard library, plus the vendored pffft behind RealFFT.
# Can be compiled and tested independently on any platform.
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)

# pffft, detras de RealFFT (MINI-058). Un solo target aunque varias suites traigan dsp.
if(NOT TARGET pffft)
    include(${CMAKE_CURRENT_LIST_DIR}/../thirdparty/pffft.cmake)
endif()

add_library(watermelon-dsp STATIC
    # Filters
    BiquadFilter.cpp
    # StateVariableFilter.h — header-only
    # EnvelopeFollower.h — header-only
    # DCBlocker.h — header-only

    # Deteccion de altura (REQ-001 S4)
    McLeodPitch.cpp

    # FFT real (MINI-058)
    RealFFT.cpp

    # Delay
    DelayLine.cpp
    StereoTools.cpp

    # Modulation / Synthesis
    LFO.cpp
    GrainEngine.cpp
    Oversampler.cpp

    # Reverb
    FDN.cpp
    EarlyReflections.cpp
    VocoderBank.cpp

    # Header-only utilities (listed for IDE awareness):
    # DSPMath.h, ParameterSmoother.h, SoftClipper.h, Dithering.h,
    # NoiseGate.h, LevelMeter.h, SIMDUtils.h,
    # LockFreeRingBuffer.h, LockFreeEventQueue.h
)

target_include_directories(watermelon-dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(watermelon-dsp PUBLIC cxx_std_20)
target_link_libraries(watermelon-dsp PRIVATE pffft)

# Propagate SIMD defines so consumers get USE_NEON/USE_SSE
if(ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_definitions(watermelon-dsp PUBLIC USE_NEON=1)
endif()
if(ANDROID_ABI STREQUAL "x86_64" OR ANDROID_ABI STREQUAL "x86")
    target_compile_definitions(watermelon-dsp PUBLIC USE_SSE=1)
endif()
