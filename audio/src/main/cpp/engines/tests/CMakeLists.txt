# ============================================================================
# watermelon-engines unit tests (host, x86_64)
#
# Header-only synth engines (FM, Supersaw, Wavetable, Granular, KarplusStrong)
# are included directly — we do NOT link watermelon-engines, because that lib
# pulls in SoundFontManager.cpp -> tinysoundfont, an Android/external dep not
# available on the host. Only watermelon-dsp is needed (DelayLine,
# ParameterSmoother). SoundFontEngine is intentionally out of host scope.
#
# Build & run standalone:
#   cd audio/src/main/cpp/engines/tests
#   mkdir -p build && cd build
#   cmake .. && cmake --build . && ctest --output-on-failure
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(watermelon-engines-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Guarded so the unified test root (../../tests) can pull this suite in
# alongside dsp/tests without defining watermelon-dsp twice.
if(NOT TARGET watermelon-dsp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dsp ${CMAKE_BINARY_DIR}/dsp)
endif()

add_executable(engine_tests
    test_engines.cpp
    test_soundfont_fd_region.cpp
)

target_include_directories(engine_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(engine_tests PRIVATE watermelon-dsp GTest::gtest_main)
target_compile_definitions(engine_tests PRIVATE _USE_MATH_DEFINES)

include(GoogleTest)
gtest_discover_tests(engine_tests)
