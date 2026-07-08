# ============================================================================
# watermelon-usb unit tests
#
# Stages 1 & 2 — USB audio backend foundations + discovery
#
# Uses Google Test. Run on host (x86_64), not on Android device.
# Standalone build, not wired into the main CMakeLists. Mirrors the pattern
# of audio/src/main/cpp/dsp/tests/CMakeLists.txt.
#
# Build & run:
#   cd audio/src/main/cpp/usb/tests
#   mkdir -p build && cd build
#   cmake .. && cmake --build . --config Debug && ctest --output-on-failure
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(watermelon-usb-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch Google Test (same version pin as the dsp test target)
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# We compile only the small slice of the audio engine that the USB unit tests
# actually need:
#   - UsbDescriptorParser.cpp  (for the parser feedback-endpoint test)
#   - Logger.cpp               (Logger.h is included transitively; the .cpp
#                               provides the stderr fallback for non-Android)
#
# ClockController, SampleRateRequest and the constants are header-only, so
# the tests pull them in directly via #include without needing extra .cpp
# files. This keeps the build dependency surface minimal — no libusb, no
# Android, no Oboe.
set(USB_TESTED_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../UsbDescriptorParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/DriftResampler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/SplitBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/Logger.cpp
)

add_executable(usb_tests
    test_clock_controller.cpp
    test_implicit_feedback.cpp
    test_rate_coercion_policy.cpp
    test_usb_latency_math.cpp
    test_sample_rate_request.cpp
    test_usb_descriptor_parser.cpp
    test_altsetting_selector.cpp
    test_snapshot_codec.cpp
    test_clock_range_parser.cpp
    test_clock_graph.cpp
    test_usb_iso_timing.cpp
    test_usb_latency_profile.cpp
    test_dsp_pacer.cpp
    test_jitter_budget_controller.cpp
    test_resizable_ring_buffer.cpp
    test_channel_map.cpp
    test_feature_unit_caps.cpp
    test_recovery_policy.cpp
    test_backend_endpoint_contract.cpp
    test_drift_resampler.cpp
    test_split_backend.cpp
    ${USB_TESTED_SOURCES}
)

target_include_directories(usb_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(usb_tests PRIVATE GTest::gtest_main)

# Register with CTest
include(GoogleTest)
gtest_discover_tests(usb_tests)
