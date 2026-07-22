# ============================================================================
# watermelon-audio — iOS build (WA-2.1)
#
# Deliberately separate from ../CMakeLists.txt, which AGP drives and which is
# Android-specific end to end (Oboe, libusb, JNI, PlatformAndroid, GNU-only
# linker flags). Keeping the iOS build here means zero regression risk for the
# shipping Android build.
#
# SCOPE TODAY — portable DSP only:
#     watermelon-dsp / -effects / -engines / -voice / -looper + tinysoundfont
#
# NOT here yet: core/, nodes/, api/, backends/. Those do not compile for iOS —
# AudioEngine.cpp and InputNode are coupled to Oboe directly (InputNode even
# inherits oboe::AudioStreamDataCallback), not just through IAudioBackend.
# Decoupling them is its own requirement and a prerequisite for WA-2.4
# (CoreAudioBackend). When that lands, add core/nodes/api here and this target
# becomes the real libwatermelon_audio.a.
#
# Excluded by design (D4 / platform): jni/, usb/, OboeBackend, LibusbBackend,
# PlatformAndroid.cpp.
#
# Build both slices with: scripts/build-ios.sh
# ============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(watermelon_audio_ios CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WMA_CPP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# Apple silicon (device and simulator alike) is arm64 — NEON is unconditional,
# unlike Android where it depends on the ABI.
if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    add_compile_definitions(USE_NEON=1)
    message(STATUS "iOS arm64: NEON enabled")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Mirrors the Android release flags, minus the GNU-only linker options
    # (--gc-sections, --strip-all, -z max-page-size) that Apple ld rejects.
    # -ffinite-math-only stays out for the same reason as on Android: it lets
    # the compiler drop the isnan/isinf guards in EffectChain.
    add_compile_options(-O3 -DNDEBUG)
    add_compile_options(
        -fno-math-errno
        -fno-trapping-math
        -fno-signed-zeros
        -freciprocal-math
        -fassociative-math
    )
    add_compile_options(-ftree-vectorize)
    message(STATUS "iOS release: audio optimizations enabled")
endif()

include(${WMA_CPP_ROOT}/thirdparty/tinysoundfont.cmake)

add_subdirectory(${WMA_CPP_ROOT}/dsp     ${CMAKE_BINARY_DIR}/dsp)
add_subdirectory(${WMA_CPP_ROOT}/effects ${CMAKE_BINARY_DIR}/effects)
add_subdirectory(${WMA_CPP_ROOT}/engines ${CMAKE_BINARY_DIR}/engines)
add_subdirectory(${WMA_CPP_ROOT}/looper  ${CMAKE_BINARY_DIR}/looper)
add_subdirectory(${WMA_CPP_ROOT}/voice   ${CMAKE_BINARY_DIR}/voice)

# Aggregate target so the slice has a single thing to build and depend on.
# CMake does not merge static archives, so the actual merge into one .a is done
# by scripts/build-ios.sh with libtool.
add_custom_target(watermelon_audio_ios ALL
    DEPENDS watermelon-dsp watermelon-effects watermelon-engines watermelon-voice tinysoundfont
)
