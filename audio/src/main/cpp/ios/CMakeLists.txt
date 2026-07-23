# ============================================================================
# watermelon-audio — iOS build (WA-2.1)
#
# Deliberately separate from ../CMakeLists.txt, which AGP drives and which is
# Android-specific end to end (Oboe, libusb, JNI, PlatformAndroid, GNU-only
# linker flags). Keeping the iOS build here means zero regression risk for the
# shipping Android build.
#
# SHIPPED TARGET — portable DSP only:
#     watermelon-dsp / -effects / -engines / -voice / -looper + tinysoundfont
#
# core/, nodes/, api/ and backends/ DO compile for iOS since WA-2.0 and WA-2.7,
# but they are still behind WMA_IOS_PROBE_CORE rather than in the shipped
# target, because two gaps remain before the archive is usable:
#
#   - nodes/InputNode.cpp — its Oboe adapter is Android-only, so it leaves ~22
#     undefined symbols. iOS needs its own input path.
#   - CoreAudioBackend (WA-2.4) — createSystemAudioBackend() returns nullptr on
#     iOS today, so the manager constructs but cannot open a stream.
#
# Note a STATIC target would NOT fail on those undefined symbols, which is why
# the probe is checked with `nm -u` rather than by whether it builds.
#
# Once WA-2.4 lands, promote the probe sources into watermelon_audio_ios and
# this becomes the real libwatermelon_audio.a.
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

# WA-2.0 probe: how much of the core compiles without Oboe. Not part of the
# shipped target yet — see the aggregate target at the bottom.
option(WMA_IOS_PROBE_CORE "Also try to compile core/nodes/api for iOS" OFF)

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

if(WMA_IOS_PROBE_CORE)
    # InputNode.cpp stays out: its Oboe adapter is Android-only. The *header* is
    # Oboe-free, so anything including it still compiles.
    add_library(watermelon-core-probe STATIC
        ${WMA_CPP_ROOT}/core/AudioEngine.cpp
        ${WMA_CPP_ROOT}/core/SynthEngineDispatcher.cpp
        ${WMA_CPP_ROOT}/core/OutputStage.cpp
        ${WMA_CPP_ROOT}/core/OscillatorBank.cpp
        ${WMA_CPP_ROOT}/core/MusicalScale.cpp
        ${WMA_CPP_ROOT}/core/XYMapper.cpp
        ${WMA_CPP_ROOT}/core/ModeManager.cpp
        ${WMA_CPP_ROOT}/core/graph/AudioGraph.cpp
        ${WMA_CPP_ROOT}/nodes/OscillatorNode.cpp
        ${WMA_CPP_ROOT}/nodes/EffectChainNode.cpp
        ${WMA_CPP_ROOT}/nodes/OutputNode.cpp
        ${WMA_CPP_ROOT}/nodes/MixerNode.cpp
        ${WMA_CPP_ROOT}/analysis/SpectrumAnalyzer.cpp
        ${WMA_CPP_ROOT}/looper/LooperExporter.cpp
        ${WMA_CPP_ROOT}/platform/Logger.cpp
        ${WMA_CPP_ROOT}/backends/BackendManager.cpp
        ${WMA_CPP_ROOT}/backends/PlatformBackends.cpp
        ${WMA_CPP_ROOT}/backends/SplitBackend.cpp
        ${WMA_CPP_ROOT}/backends/DriftResampler.cpp
        ${WMA_CPP_ROOT}/api/watermelon_audio.cpp
    )
    target_include_directories(watermelon-core-probe PRIVATE ${WMA_CPP_ROOT})
    target_link_libraries(watermelon-core-probe
        PUBLIC watermelon-voice watermelon-looper watermelon-engines
               watermelon-effects watermelon-dsp tinysoundfont
    )
endif()
