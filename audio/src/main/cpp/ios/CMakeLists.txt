# ============================================================================
# watermelon-audio — iOS build (WA-2.1)
#
# Deliberately separate from ../CMakeLists.txt, which AGP drives and which is
# Android-specific end to end (Oboe, libusb, JNI, PlatformAndroid, GNU-only
# linker flags). Keeping the iOS build here means zero regression risk for the
# shipping Android build.
#
# SHIPPED TARGET — the full engine:
#     watermelon-core (core/, nodes/, api/, backends/, platform/, analysis/)
#     + watermelon-dsp / -effects / -engines / -voice / -looper + tinysoundfont
#
# The core used to sit behind a WMA_IOS_PROBE_CORE option because two gaps made
# the archive unusable: InputNode.cpp had no non-Oboe path, and there was no
# iOS backend to open a stream with. WA-2.2 and WA-2.4 closed both, so the probe
# was promoted here — this is now the real libwatermelon_audio.a, and it is what
# cinterop links against (WA-3.1).
#
# A STATIC target does NOT fail on undefined symbols, so "it builds" proves
# nothing about linkability. The real check is `nm -u` on the merged archive —
# see scripts/build-ios.sh, which fails the build if a project symbol is left
# unresolved.
#
# Still missing on iOS, and NOT a link gap: audio capture. InputNode's
# createInputStream()/startInputStream() return false on any platform without
# Oboe, so the node exists and processes but never has a live input stream.
# Full-duplex / guitar FX on iOS needs a CoreAudio capture adapter at that seam.
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

# ----------------------------------------------------------------------------
# Gate de warnings.
#
# Colocado DESPUES de tinysoundfont —para no atar al third-party a nuestras
# reglas— y ANTES de los add_subdirectory y del target de abajo, para que herede
# a todo lo propio. Misma politica, mismas banderas y mismo `-Wno-unused-parameter`
# que tests/CMakeLists.txt, con la misma razon: este motor tiene muchos stubs de
# interfaz que ignoran parametros legitimamente, y marcarlos es ruido.
#
# Lo que compra, y es la unica razon por la que esta: los cuatro archivos que el
# build de iOS compila y la suite de host NO —CoreAudioBackend.mm, PlatformApple.cpp,
# PlatformBackends.cpp y el InputNode real (la suite le pone stubs)— no pasaban por
# ningun gate de warnings bajo ningun compilador. El CMakeLists que maneja AGP
# tampoco tiene banderas. Era el codigo mas nuevo del repo y el unico Objective-C++,
# sin diagnostico.
#
# Lo que NO compra: no reemplaza a la suite de host de macOS. Esa compila 84
# archivos que este build nunca ve (los ~78 de test, usb/, PlatformAndroid.cpp) y,
# sobre todo, EJECUTA los 779 tests bajo Apple clang + libc++; un archivo estatico
# cross-compilado no ejecuta nada. Son complementarios, no sustitutos.
#
# Verificado al introducirlo: los dos slices, en Debug y en Release, compilan con
# CERO warnings — o sea que el gate entra sin deuda que pagar.
option(WMA_IOS_WERROR "Treat iOS build warnings as errors" ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(WMA_IOS_WERROR)
    add_compile_options(-Werror)
endif()

add_subdirectory(${WMA_CPP_ROOT}/dsp     ${CMAKE_BINARY_DIR}/dsp)
add_subdirectory(${WMA_CPP_ROOT}/effects ${CMAKE_BINARY_DIR}/effects)
add_subdirectory(${WMA_CPP_ROOT}/engines ${CMAKE_BINARY_DIR}/engines)
add_subdirectory(${WMA_CPP_ROOT}/looper  ${CMAKE_BINARY_DIR}/looper)
add_subdirectory(${WMA_CPP_ROOT}/voice   ${CMAKE_BINARY_DIR}/voice)

# The full engine as it builds for iOS. Everything the C API needs to stand up
# an AudioEngine, minus the Android-only I/O (see the exclusions at the top).
add_library(watermelon-core STATIC
        ${WMA_CPP_ROOT}/core/AudioEngine.cpp
        ${WMA_CPP_ROOT}/core/SynthEngineDispatcher.cpp
        ${WMA_CPP_ROOT}/core/OutputStage.cpp
        ${WMA_CPP_ROOT}/core/OscillatorBank.cpp
        ${WMA_CPP_ROOT}/core/MusicalScale.cpp
        ${WMA_CPP_ROOT}/core/XYMapper.cpp
        ${WMA_CPP_ROOT}/nodes/OscillatorNode.cpp
        ${WMA_CPP_ROOT}/nodes/EffectChainNode.cpp
        ${WMA_CPP_ROOT}/nodes/MixerNode.cpp
        ${WMA_CPP_ROOT}/nodes/InputNode.cpp
        ${WMA_CPP_ROOT}/analysis/SpectrumAnalyzer.cpp
        ${WMA_CPP_ROOT}/looper/LooperExporter.cpp
        ${WMA_CPP_ROOT}/platform/Logger.cpp
        ${WMA_CPP_ROOT}/platform/PlatformApple.cpp
        ${WMA_CPP_ROOT}/backends/BackendManager.cpp
        ${WMA_CPP_ROOT}/backends/PlatformBackends.cpp
        ${WMA_CPP_ROOT}/backends/SplitBackend.cpp
        ${WMA_CPP_ROOT}/backends/DriftResampler.cpp
        ${WMA_CPP_ROOT}/backends/CoreAudioBackend.mm
        ${WMA_CPP_ROOT}/api/watermelon_audio.cpp
)
target_include_directories(watermelon-core PRIVATE ${WMA_CPP_ROOT})

# CoreAudioBackend.mm is Objective-C++ (clang detects .mm automatically) and
# relies on ARC for the AVAudioEngine object lifetime — enable it per-file.
set_source_files_properties(
    ${WMA_CPP_ROOT}/backends/CoreAudioBackend.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
)

# Apple audio frameworks. CoreAudio is iOS/macOS; AVFoundation + AudioToolbox
# + Foundation carry AVAudioEngine and the AudioBufferList types.
#
# These are usage requirements only: a STATIC archive links nothing. Whoever
# consumes the .a has to link them too — for Kotlin/Native that means
# `-framework AVFoundation …` in the cinterop .def (WA-3.1).
find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
find_library(COREAUDIO_FRAMEWORK    CoreAudio    REQUIRED)
find_library(FOUNDATION_FRAMEWORK   Foundation   REQUIRED)

target_link_libraries(watermelon-core
    PUBLIC watermelon-voice watermelon-looper watermelon-engines
           watermelon-effects watermelon-dsp tinysoundfont
    PRIVATE ${AVFOUNDATION_FRAMEWORK} ${AUDIOTOOLBOX_FRAMEWORK}
            ${COREAUDIO_FRAMEWORK} ${FOUNDATION_FRAMEWORK}
)

# Aggregate target so the slice has a single thing to build and depend on.
# watermelon-core links the sub-libraries PUBLIC, so depending on it pulls the
# whole set. CMake does not merge static archives — scripts/build-ios.sh does
# that with libtool.
add_custom_target(watermelon_audio_ios ALL)
add_dependencies(watermelon_audio_ios watermelon-core)
