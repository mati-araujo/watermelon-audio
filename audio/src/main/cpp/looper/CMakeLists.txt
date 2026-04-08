# ============================================================================
# watermelon-looper: Audio track looper sub-library
#
# Phase 1D.2 — Audio Library Extraction
#
# Header-only library: AudioLooper.h, TrackBuffer.h, WavFile.h
# Depends on: watermelon-dsp (ParameterSmoother, SIMDUtils, LockFreeRingBuffer)
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)

# Header-only — use INTERFACE library
add_library(watermelon-looper INTERFACE)
target_include_directories(watermelon-looper INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(watermelon-looper INTERFACE watermelon-dsp)
