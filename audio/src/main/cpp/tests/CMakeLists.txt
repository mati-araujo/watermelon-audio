# ============================================================================
# watermelon-audio — unified host test suite
#
# Aggregates every host-side (x86_64) Google Test suite into ONE configure /
# build / ctest run, sharing a single googletest download. Runs on the dev
# host (Windows/Linux/macOS) — NOT on an Android device. No Oboe, no JNI,
# no NDK required.
#
#   dsp_tests      — audio/src/main/cpp/dsp/tests
#   effects_tests  — audio/src/main/cpp/effects/tests
#   looper_tests   — audio/src/main/cpp/looper/tests
#   usb_tests      — audio/src/main/cpp/usb/tests
#
# Prefer the wrapper scripts, which locate a host toolchain (MinGW g++ +
# Ninja) for you:
#
#   pwsh   scripts/run-cpp-tests.ps1      (Windows)
#   bash   scripts/run-cpp-tests.sh       (Linux / CI / macOS)
#
# Manual equivalent:
#   cmake -S audio/src/main/cpp/tests -B audio/src/main/cpp/tests/build -G Ninja
#   cmake --build audio/src/main/cpp/tests/build
#   ctest --test-dir audio/src/main/cpp/tests/build --output-on-failure
#
# Each child CMakeLists still builds standalone (cd into it + cmake) — the
# `if(NOT TARGET ...)` guards added to dsp/ and effects/ make the shared
# library subdirectories safe to pull in more than once from here.
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(watermelon-cpp-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

# Fetch googletest once, here. The child suites also declare it, but
# FetchContent dedupes by name, so this is the single source of truth and
# the only network hit. Pin matches every child suite (v1.15.2).
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ----------------------------------------------------------------------------
# Warning gate — host test build ONLY (never the NDK/production build).
#
# Placed AFTER googletest is fetched so third-party code is not held to it, and
# BEFORE the test suites so it propagates to them and to the watermelon-* libs
# they compile. This is what catches real defects early (e.g. the uninitialized
# `a0` in FilterEffect). Turn the gate off locally with -DWMA_TESTS_WERROR=OFF.
# ----------------------------------------------------------------------------
# -Wno-unused-parameter: this engine has many interface defaults / override
# stubs that legitimately ignore parameters; flagging them is noise. The
# high-value checks (uninitialized, reorder, format, unused-variable) stay on.
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

if(WIN32)
    # MinGW's default (msvcrt) printf rejects C99 %zu/%lld length modifiers that
    # are valid on the Android/bionic target, producing host-only false -Wformat
    # positives. This switches MinGW to its C99-compliant stdio so -Wformat
    # validates against the real target semantics instead of blocking the gate.
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

option(WMA_TESTS_WERROR "Treat host-test warnings as errors" ON)
if(WMA_TESTS_WERROR)
    add_compile_options(-Werror)
endif()

# Order is irrelevant — the shared-library subdirectories are guarded.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../dsp/tests     dsp_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../effects/tests effects_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../engines/tests engine_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../voice/tests   voice_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../looper/tests  looper_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../usb/tests     usb_tests)
