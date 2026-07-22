# ============================================================================
# watermelon-dsp unit tests
#
# Phase 1G — Audio Library Extraction
#
# Uses Google Test. Run on host (x86_64), not on Android device.
#
# Build & run:
#   cd audio/src/main/cpp/dsp/tests
#   mkdir -p build && cd build
#   cmake .. && make -j$(nproc) && ctest --output-on-failure
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(watermelon-dsp-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Build watermelon-dsp as a static library (parent directory).
# Guarded so the unified test root (../tests) can pull this suite in
# alongside effects/tests without defining watermelon-dsp twice.
if(NOT TARGET watermelon-dsp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_BINARY_DIR}/dsp)
endif()

# Test executable
add_executable(dsp_tests
    test_biquad_filter.cpp
    test_parameter_smoother.cpp
    test_soft_clipper.cpp
    test_lock_free_ring_buffer.cpp
    test_delay_line.cpp
    test_fdn.cpp
    test_lfo.cpp
    test_dc_blocker.cpp
    test_dsp_math.cpp
    test_noise_gate.cpp
)

target_link_libraries(dsp_tests PRIVATE watermelon-dsp GTest::gtest_main)
target_compile_definitions(dsp_tests PRIVATE _USE_MATH_DEFINES)

# Register with CTest
include(GoogleTest)
gtest_discover_tests(dsp_tests)
