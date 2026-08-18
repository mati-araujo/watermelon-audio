# ============================================================================
# core/AudioEngine unit tests (dev host — no NDK, no Oboe, no device)
#
# The first host suite for core/. It exists because WA-2.0 + WA-2.7 removed the
# two things that made AudioEngine untestable off-device: AudioEngine.cpp no
# longer includes Oboe outside __ANDROID__, and BackendManager no longer names a
# concrete backend.
#
# Scope: the BackendManager audio path — the one USB uses today and CoreAudio
# will use on iOS. The legacy direct-Oboe path is Android-only by construction
# (WMA_HAS_OBOE) and stays out of host scope.
#
# TWO SOURCES ARE SUBSTITUTED, both under support/ and both marked as stubs:
#
#   backends/PlatformBackends.cpp -> support/test_platform_backends.cpp
#       Hands BackendManager a FakeAudioBackend. The real file returns nullptr
#       off Android, which leaves nothing able to report a running stream.
#
#   nodes/InputNode.cpp           -> support/test_input_node_stub.cpp
#       Does not compile on the host (its capture adapter inherits from
#       oboe::AudioStreamDataCallback). Link-time bodies only, no behaviour.
#
# Build & run standalone:
#   cd audio/src/main/cpp/core/tests
#   mkdir -p build && cd build
#   cmake .. && cmake --build . && ctest --output-on-failure
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(watermelon-core-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Harmless when this directory is pulled in by ../../tests (which already
# enabled testing); required for the standalone flow documented above to
# actually register the discovered tests with CTest.
enable_testing()

include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(WMA_CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Guarded so the unified test root (../../tests) can pull this suite in
# alongside the other suites without defining the shared libs twice.
if(NOT TARGET watermelon-dsp)
    add_subdirectory(${WMA_CPP_ROOT}/dsp     ${CMAKE_BINARY_DIR}/dsp)
endif()
if(NOT TARGET watermelon-effects)
    add_subdirectory(${WMA_CPP_ROOT}/effects ${CMAKE_BINARY_DIR}/effects)
endif()
if(NOT TARGET watermelon-engines)
    add_subdirectory(${WMA_CPP_ROOT}/engines ${CMAKE_BINARY_DIR}/engines)
endif()
if(NOT TARGET watermelon-voice)
    add_subdirectory(${WMA_CPP_ROOT}/voice   ${CMAKE_BINARY_DIR}/voice)
endif()
if(NOT TARGET watermelon-looper)
    add_subdirectory(${WMA_CPP_ROOT}/looper  ${CMAKE_BINARY_DIR}/looper)
endif()
if(NOT TARGET tinysoundfont)
    include(${WMA_CPP_ROOT}/thirdparty/tinysoundfont.cmake)
endif()

# The slice of the engine AudioEngine.cpp needs to link. Mirrors the
# watermelon-core target in ios/CMakeLists.txt, with the two substitutions
# described in the header above.
#
# api/ joined the target in WA-2.6 (category `lifecycle`): once the JNI became a
# one-line wrapper over the C API, the behaviour the JNI used to own — the value
# a query returns with no engine, the difference between the engine's default
# fade and an explicit one — lives in watermelon_audio.cpp. Testing AudioEngine
# alone would no longer cover it. See test_c_api_lifecycle.cpp.
set(CORE_TESTED_SOURCES
    ${WMA_CPP_ROOT}/api/watermelon_audio.cpp
    ${WMA_CPP_ROOT}/core/AudioEngine.cpp
    ${WMA_CPP_ROOT}/core/SynthEngineDispatcher.cpp
    ${WMA_CPP_ROOT}/core/OutputStage.cpp
    ${WMA_CPP_ROOT}/core/OscillatorBank.cpp
    ${WMA_CPP_ROOT}/core/MusicalScale.cpp
    ${WMA_CPP_ROOT}/core/XYMapper.cpp
    ${WMA_CPP_ROOT}/nodes/OscillatorNode.cpp
    ${WMA_CPP_ROOT}/nodes/EffectChainNode.cpp
    ${WMA_CPP_ROOT}/nodes/MixerNode.cpp
    ${WMA_CPP_ROOT}/analysis/SpectrumAnalyzer.cpp
    ${WMA_CPP_ROOT}/looper/LooperExporter.cpp
    ${WMA_CPP_ROOT}/platform/Logger.cpp
    # Despite the name, this file is plain POSIX + inline asm with the only
    # __ANDROID__ guard inside setAudioThreadPriority(). AudioEngine::start()
    # calls flushDenormals(), so the real implementation is compiled in rather
    # than stubbed — one less divergence between host and device behaviour.
    ${WMA_CPP_ROOT}/platform/PlatformAndroid.cpp
    ${WMA_CPP_ROOT}/backends/BackendManager.cpp
    ${WMA_CPP_ROOT}/backends/SplitBackend.cpp
    ${WMA_CPP_ROOT}/backends/DriftResampler.cpp
)

add_executable(core_tests
    test_current_sample_rate.cpp
    test_output_stage_protection.cpp
    test_denormal_flush.cpp
    test_input_node_retire.cpp
    test_offline_render.cpp
    test_fade_lifecycle.cpp
    test_capture_requests.cpp
    test_c_api_lifecycle.cpp
    test_c_api_master_bus.cpp
    test_c_api_input.cpp
    test_c_api_effects.cpp
    test_c_api_synth.cpp
    test_soundfont_load.cpp
    test_c_api_voice.cpp
    test_c_api_mode.cpp
    test_c_api_analysis.cpp
    test_c_api_transport.cpp
    test_c_api_diagnostics.cpp
    test_c_api_looper.cpp
    test_c_api_routing.cpp
    test_c_api_logcapture.cpp
    test_c_api_start_without_select.cpp
    support/test_platform_backends.cpp
    support/test_input_node_stub.cpp
    ${CORE_TESTED_SOURCES}
)

target_include_directories(core_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WMA_CPP_ROOT}
)

# El bloque que demotaba -Wunused-variable / -Wunused-private-field para este
# target se fue: las tres declaraciones muertas que lo justificaban —el
# `rootNote` de MusicalScale, el `normalizedPos` de BurstModulator y el
# `mLastBpm` de AudioEngine— estan borradas, asi que el -Werror de
# ../../tests/CMakeLists.txt aplica a core/ en full, que es lo que ese comentario
# pedia. Si vuelve a hacer falta demotarlo, el valor a mirar es por que.

find_package(Threads REQUIRED)
target_link_libraries(core_tests PRIVATE
    watermelon-voice
    watermelon-looper
    watermelon-engines
    watermelon-effects
    watermelon-dsp
    tinysoundfont
    GTest::gtest_main
    Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(core_tests)
