# ============================================================================
# watermelon-looper unit tests
#
# Sprint 4 — looper test suite covering: WAV writer/reader round-trip,
# Limiter peak control, Transport quantization, TrackBuffer recording +
# tail-buffer playback, PreRollRing snapshot semantics.
#
# Standalone build (mirrors usb/tests pattern). Runs on host (x86_64),
# not on Android device — no Oboe, no JNI, no platform Logger.
#
# Build & run:
#   cd audio/src/main/cpp/looper/tests
#   mkdir -p build && cd build
#   cmake .. && cmake --build . --config Debug && ctest --output-on-failure
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(watermelon-looper-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch Google Test (same pin as usb/tests).
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Shared sources from the engine (Logger.cpp for the stderr fallback that
# the looper headers include via platform/Logger.h).
set(LOOPER_TESTED_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/Logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../LooperExporter.cpp
)

add_executable(looper_tests
    test_wav_file.cpp
    test_limiter.cpp
    test_transport.cpp
    test_track_buffer.cpp
    test_audio_looper.cpp
    test_chunked_buffer.cpp
    test_looper_stress.cpp
    test_free_loop_autosync.cpp
    test_pre_roll_ring.cpp
    test_pan_lut.cpp
    ${LOOPER_TESTED_SOURCES}
)

target_include_directories(looper_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(looper_tests PRIVATE GTest::gtest_main)

# Define M_PI on platforms that don't (MSVC/MinGW).
target_compile_definitions(looper_tests PRIVATE _USE_MATH_DEFINES)

include(GoogleTest)
gtest_discover_tests(looper_tests)

# ----------------------------------------------------------------------------
# looper_tests_chunked — the storage-agnostic TrackBuffer suite compiled with
# WM_LOOPER_CHUNKED_BUFFER, so the paged backend is verified against the SAME
# assertions as the dense default (plan F2.2). Uses only tests that go through
# TrackBuffer's public API (no exporter, which is chunk-ready in F2.3).
# ----------------------------------------------------------------------------
add_executable(looper_tests_chunked
    test_track_buffer.cpp
    test_free_loop_autosync.cpp
    test_chunked_buffer.cpp
    test_audio_looper.cpp
    test_looper_stress.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../LooperExporter.cpp        # now storage-agnostic (F2.3)
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/Logger.cpp
)
target_include_directories(looper_tests_chunked PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
target_link_libraries(looper_tests_chunked PRIVATE GTest::gtest_main)
target_compile_definitions(looper_tests_chunked PRIVATE
    _USE_MATH_DEFINES WM_LOOPER_CHUNKED_BUFFER)
gtest_discover_tests(looper_tests_chunked)

# ----------------------------------------------------------------------------
# bench_mix_into — standalone micro-benchmark for TrackBuffer::mixInto (F0).
# NOT registered with ctest: it's a manual PR-time metric, not a gate. Build in
# Release for meaningful numbers. Depends only on TrackBuffer.h + Logger.
# ----------------------------------------------------------------------------
add_executable(bench_mix_into
    bench_mix_into.cpp
    ${LOOPER_TESTED_SOURCES}
)
target_include_directories(bench_mix_into PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
target_compile_definitions(bench_mix_into PRIVATE _USE_MATH_DEFINES)
