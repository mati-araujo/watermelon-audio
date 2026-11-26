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
# El pin de googletest vive en UN solo lugar (REQ-047 S4).
include(${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/googletest.cmake)

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
    test_master_stage.cpp
    test_real_fft.cpp
    # REQ-055 S12: cuenta las allocations del hilo ("0 alloc en processMaster")
    ../../core/tests/support/rtalloc_counter.cpp
)

target_link_libraries(dsp_tests PRIVATE watermelon-dsp GTest::gtest_main)
target_compile_definitions(dsp_tests PRIVATE _USE_MATH_DEFINES)

# MINI-058: RtAllocCounter no ve `malloc` y pffft es C. Donde el linker sabe `--wrap`
# (GNU ld y lld; no Apple ld), el test de RealFFT cuenta tambien malloc/calloc/realloc.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LINK_OPTIONS -Wl,--wrap=malloc)
check_cxx_source_compiles("#include <cstdlib>
extern \"C\" void* __real_malloc(std::size_t);
extern \"C\" void* __wrap_malloc(std::size_t n) { return __real_malloc(n); }
int main() { void* p = std::malloc(1); std::free(p); return 0; }" WMA_LINKER_HAS_WRAP)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(WMA_LINKER_HAS_WRAP)
    target_compile_definitions(dsp_tests PRIVATE WMA_WRAP_MALLOC=1)
    target_link_options(dsp_tests PRIVATE
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
endif()

# Register with CTest
include(GoogleTest)
wma_gtest_discover_tests(dsp_tests)
