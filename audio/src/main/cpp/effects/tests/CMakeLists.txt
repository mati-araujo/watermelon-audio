cmake_minimum_required(VERSION 3.22.1)
project(watermelon-effects-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Guarded so the unified test root (../tests) can pull this suite in
# alongside dsp/tests without defining these libraries twice.
if(NOT TARGET watermelon-dsp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dsp ${CMAKE_BINARY_DIR}/dsp)
endif()
if(NOT TARGET watermelon-effects)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_BINARY_DIR}/effects)
endif()

add_executable(effects_tests
    test_guitar_delay_reverb.cpp
    test_filter_compressor.cpp
    test_distortion_amp.cpp
    test_distortion_gate.cpp
    test_modulation.cpp
    test_misc_effects.cpp
    test_logger_stub.cpp
)

target_include_directories(effects_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../dsp
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(effects_tests PRIVATE watermelon-effects GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(effects_tests)
