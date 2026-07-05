# ============================================================================
# watermelon-voice unit tests (host, x86_64)
#
# Scope: TouchTriggerSource — the touch -> VoiceTriggerEvent mapping and its
# lock-free event queue. This is the part of voice/ that has no dependency on
# core/AudioSource or concrete oscillators.
#
# NOT covered on host: Voice / VoicePool / VoiceManager. Voice owns concrete
# AudioSource oscillators and watermelon-voice links watermelon-engines
# (-> tinysoundfont), so they can't link on the host without decoupling from
# the oscillator/engine concretes (see the Voice test-seam note in the sweep).
#
# Build & run standalone:
#   cd audio/src/main/cpp/voice/tests
#   mkdir -p build && cd build && cmake .. && cmake --build . && ctest
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(watermelon-voice-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# LockFreeEventQueue lives in watermelon-dsp; guarded for the unified root.
if(NOT TARGET watermelon-dsp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dsp ${CMAKE_BINARY_DIR}/dsp)
endif()

add_executable(voice_tests
    test_touch_trigger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../TouchTriggerSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/Logger.cpp
)

target_include_directories(voice_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(voice_tests PRIVATE watermelon-dsp GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(voice_tests)
