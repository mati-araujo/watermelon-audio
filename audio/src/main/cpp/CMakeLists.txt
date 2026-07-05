cmake_minimum_required(VERSION 3.22.1)
project("watermelon_audio")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ========== VERSION FROM GRADLE ==========
# Passed via -DWMA_PROJECT_VERSION=... from audio/build.gradle.kts.
# Falls back to hardcoded values in watermelon_audio.h for standalone builds.
if(DEFINED WMA_PROJECT_VERSION AND NOT WMA_PROJECT_VERSION STREQUAL "unspecified")
    string(REPLACE "." ";" _WMA_VERSION_PARTS "${WMA_PROJECT_VERSION}")
    list(LENGTH _WMA_VERSION_PARTS _WMA_VERSION_PARTS_LEN)
    if(_WMA_VERSION_PARTS_LEN GREATER_EQUAL 2)
        list(GET _WMA_VERSION_PARTS 0 _WMA_VER_MAJOR)
        list(GET _WMA_VERSION_PARTS 1 _WMA_VER_MINOR)
        if(_WMA_VERSION_PARTS_LEN GREATER 2)
            list(GET _WMA_VERSION_PARTS 2 _WMA_VER_PATCH_RAW)
            string(REGEX REPLACE "[^0-9].*" "" _WMA_VER_PATCH "${_WMA_VER_PATCH_RAW}")
        else()
            set(_WMA_VER_PATCH "0")
        endif()
        add_compile_definitions(
            WMA_VERSION_MAJOR=${_WMA_VER_MAJOR}
            WMA_VERSION_MINOR=${_WMA_VER_MINOR}
            WMA_VERSION_PATCH=${_WMA_VER_PATCH}
            WMA_VERSION_STRING="${WMA_PROJECT_VERSION}"
        )
        message(STATUS "WMA version from Gradle: ${WMA_PROJECT_VERSION}")
    endif()
endif()

# ========== USB DIAGNOSTICS FLAG (Fase 1, L8) ==========
# Expensive USB diagnostics (per-callback output fingerprint, periodic
# decode-back + hexdump, verbose USB_DSP log) are compiled out unless explicitly
# enabled. At 1 ms iso transfers these run up to 1000×/s and contaminate the CPU
# measurement; the cheap counters (underruns/overruns delta, ring min/max,
# clock-health line) stay always on. OFF by default — turn on with
# -DWMA_USB_DIAG=ON for a debug measurement session.
option(WMA_USB_DIAG "Enable expensive USB audio diagnostics" OFF)
if(WMA_USB_DIAG)
    add_compile_definitions(WMA_USB_DIAG=1)
    message(STATUS "WMA_USB_DIAG enabled — expensive USB diagnostics compiled in")
else()
    add_compile_definitions(WMA_USB_DIAG=0)
endif()

# Configurar alineación de 16 KB para compatibilidad con Android 15+
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

# ========== PROFESSIONAL AUDIO OPTIMIZATION FLAGS ==========
# Optimizaciones para release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Base optimization level
    add_compile_options(-O3 -DNDEBUG)

    # Safe floating-point optimizations (replaces unsafe -ffast-math)
    # These provide most of -ffast-math benefits without breaking IEEE compliance
    # NOTE: -ffinite-math-only removed intentionally — it allows the compiler to
    # eliminate std::isnan()/std::isinf() checks, which breaks our NaN/Inf
    # sanitization in EffectChain::processOneEffect() (Phase 2C P0 fix).
    add_compile_options(
        -fno-math-errno          # Don't set errno for math functions (safe for audio)
        -fno-trapping-math       # Assume no FP traps (safe for audio)
        -fno-signed-zeros        # -0.0 == +0.0 (safe for audio)
        -freciprocal-math        # Allow x/y -> x * (1/y) (safe, improves division)
        -fassociative-math       # Allow reassociation (safe for most audio DSP)
    )

    # Auto-vectorization (helps compiler use NEON automatically)
    add_compile_options(-ftree-vectorize)

    # Dead code elimination (reduces binary size)
    add_compile_options(-ffunction-sections -fdata-sections)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections")

    # Strip debug symbols for release
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--strip-all")

    # Link Time Optimization (cross-TU inlining, better optimization)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

    message(STATUS "Release build: Professional audio optimizations enabled")
endif()

# ========== ARM-SPECIFIC OPTIMIZATIONS ==========
# ARM64 (arm64-v8a): NEON is always available, optimize for common baseline
if(ANDROID_ABI STREQUAL "arm64-v8a")
    # Target Cortex-A53 as common baseline (covers most Android devices)
    # This enables NEON optimizations appropriate for this CPU
    add_compile_options(-mcpu=cortex-a53)
    # Enable NEON intrinsics header
    add_definitions(-DUSE_NEON=1)
    message(STATUS "ARM64: NEON optimizations enabled (cortex-a53 baseline)")
endif()

# ARMv7 (32-bit): Explicitly enable NEON
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    add_compile_options(-mfpu=neon -mfloat-abi=softfp)
    add_definitions(-DUSE_NEON=1)
    message(STATUS "ARMv7: NEON explicitly enabled")
endif()

# x86_64 (emulator): Enable SSE4.2 for similar SIMD capabilities
if(ANDROID_ABI STREQUAL "x86_64")
    add_compile_options(-msse4.2)
    add_definitions(-DUSE_SSE=1)
    message(STATUS "x86_64: SSE4.2 enabled for emulator")
endif()

find_package(oboe REQUIRED CONFIG)

# ========== THIRD-PARTY DEPENDENCIES ==========
# libusb for USB Audio low-latency driver
include(${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libusb.cmake)

# TinySoundFont for SoundFont2 (.sf2) sample-based synthesis (Phase 8)
include(${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tinysoundfont.cmake)

# DSP primitives sub-library (Phase 1A: audio lib extraction)
add_subdirectory(dsp)

# Effects sub-library (Phase 1B: audio lib extraction)
add_subdirectory(effects)

# Engines sub-library (Phase 1C: audio lib extraction)
add_subdirectory(engines)

# Looper sub-library (Phase 1D.2: audio lib extraction)
add_subdirectory(looper)

# Voice system sub-library (Phase 1D: audio lib extraction)
add_subdirectory(voice)

# Archivos del Audio Graph (Etapa 1)
set(GRAPH_SOURCES
        core/graph/AudioGraph.cpp
        nodes/OscillatorNode.cpp
        nodes/EffectChainNode.cpp
        nodes/OutputNode.cpp
)

# Archivos de Input (Etapa 2)
set(INPUT_SOURCES
        nodes/InputNode.cpp
)

# Archivos de Mode System (Etapa 3)
set(MODE_SOURCES
        nodes/MixerNode.cpp
        core/XYMapper.cpp
        core/ModeManager.cpp
)

# Voice System now in watermelon-voice sub-library (Phase 1D)

# Platform Abstraction Layer (Phase 0A: audio lib extraction)
set(PLATFORM_SOURCES
        platform/Logger.cpp
        platform/PlatformAndroid.cpp
)

# C API (Phase 0C: audio lib extraction)
set(API_SOURCES
        api/watermelon_audio.cpp
)

# Backend System (USB Audio Low Latency Driver)
set(BACKEND_SOURCES
        backends/OboeBackend.cpp
        backends/BackendManager.cpp
        backends/LibusbBackend.cpp
        backends/DriftResampler.cpp
        backends/SplitBackend.cpp
)

# USB Audio Driver (Phase 1 & 2 - Fundamentos y Transferencias)
set(USB_SOURCES
        usb/UsbDescriptorParser.cpp
        usb/AudioFormatConverter.cpp
        usb/UsbTransferManager.cpp
        usb/UsbVolumeControl.cpp
)

# JNI Bridge Sources (Phase 3 - Unified bridge)
set(JNI_SOURCES
        jni/jni_engine.cpp
        jni/jni_usb.cpp
        jni/jni_benchmark.cpp
        jni/jni_audio_bridge.cpp
)

add_library(watermelon_audio SHARED
        ${PLATFORM_SOURCES}
        ${API_SOURCES}
        ${JNI_SOURCES}
        core/AudioEngine.cpp
        core/SynthEngineDispatcher.cpp
        core/OutputStage.cpp
        core/OscillatorBank.cpp
        # Looper offline export/import bodies (moved out of AudioLooper.h — plan §3.3)
        looper/LooperExporter.cpp
        # Effects now in watermelon-effects sub-library (Phase 1B)
        # Audio Graph (Etapa 1)
        ${GRAPH_SOURCES}
        # Input Node (Etapa 2)
        ${INPUT_SOURCES}
        # Mode System (Etapa 3)
        ${MODE_SOURCES}
        # Voice System now in watermelon-voice sub-library (Phase 1D)
        # Backend System (USB Audio - Phase 1)
        ${BACKEND_SOURCES}
        # USB Audio Driver (Phase 1)
        ${USB_SOURCES}
        # Pro Features (Phase 5)
        core/MusicalScale.cpp
        analysis/SpectrumAnalyzer.cpp
        # Engines now in watermelon-engines sub-library (Phase 1C)
)

# Include directories for JNI headers
target_include_directories(watermelon_audio PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/jni
)

target_link_libraries(watermelon_audio
        watermelon-voice
        watermelon-looper
        watermelon-engines
        watermelon-effects
        watermelon-dsp
        oboe::oboe
        libusb::libusb
        tinysoundfont
        android
        log)

# Testing
enable_testing()

add_executable(EffectTest utils/EffectTest.cpp)
target_link_libraries(EffectTest watermelon_audio)
add_test(NAME EffectTest COMMAND EffectTest)
