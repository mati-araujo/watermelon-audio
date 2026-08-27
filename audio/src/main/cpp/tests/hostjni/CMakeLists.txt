# ============================================================================
# watermelon-audio — la libreria nativa del ARNES JNI de host (REQ-016)
#
# Produce `libwatermelon_audio.{so,dylib}` para el HOST: el mismo nombre que
# `System.loadLibrary("watermelon_audio")` busca, de modo que un test de JVM
# (`:audio:testDebugUnitTest`) pueda entrar por `AudioNativeBridge` y EJECUTAR
# funciones JNIEXPORT reales contra un JNIEnv real.
#
# POR QUE EXISTE, en una linea: 310 funciones `JNIEXPORT` y ningun test las
# ejecutaba. El gate de simbolos (MINI-001) compara NOMBRES; un desajuste de
# FIRMA compila de los dos lados, pasa ese gate y corrompe memoria en el device.
# Esto es lo unico que cruza la frontera de verdad.
#
# 🔴 LO QUE ESTA LIBRERIA NO ES: audio en dispositivo. Lleva adentro el
# `FakeAudioBackend` de core/tests (misma sustitucion de una sola TU que usa la
# suite de host, WA-2.7). Valida la frontera JNI/Kotlin y nada mas. Confundirla
# con verificacion en device seria exactamente la falsa sensacion de cobertura
# que REQ-016 existe para borrar.
#
# Es un proyecto CMake APARTE de ../CMakeLists.txt a proposito: aquel agrega las
# ocho suites de googletest y lo corre `run-cpp-tests.sh` —tambien bajo ASan y
# TSan—, y esta libreria no tiene nada que hacer ahi. Se configura sola desde
# Gradle (ver `hostJniLib` en audio/build.gradle.kts).
#
# Manual:
#   cmake -S audio/src/main/cpp/tests/hostjni -B <build> -DCMAKE_BUILD_TYPE=Debug
#   cmake --build <build>
# ============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(watermelon-host-jni CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Las sub-librerias son ARCHIVOS ESTATICOS y terminan dentro de un .so: sin esto
# el link falla en Linux con "recompile with -fPIC". En macOS no hace falta, y
# por eso es justo la clase de cosa que solo se ve midiendo en Linux.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(WMA_CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# El jni.h del JDK, no el del NDK. La unica incompatibilidad entre los dos esta
# resuelta en jni_common.h (`wmaAttachCurrentThreadAsDaemon`), sin #ifdef.
#
# A mano y NO con `find_package(JNI)`, por una razon medida: esta libreria la
# CARGA una JVM, asi que necesita los headers y jamas libjvm — y find_package(JNI)
# busca ademas AWT y el runtime, y falla de plano contra un JDK headless
# (`openjdk-17-jdk-headless` en ubuntu, que es la clase de imagen de un runner).
# `jni_md.h` vive en un subdirectorio por plataforma (darwin/, linux/), asi que se
# ubica buscandolo en vez de nombrando la plataforma.
set(WMA_JAVA_HOME "$ENV{JAVA_HOME}" CACHE PATH "JDK del que salen jni.h y jni_md.h")
if(NOT WMA_JAVA_HOME OR NOT EXISTS "${WMA_JAVA_HOME}/include/jni.h")
    message(FATAL_ERROR
        "No encuentro jni.h. Pasa -DWMA_JAVA_HOME=<jdk> o exporta JAVA_HOME "
        "(observado: '${WMA_JAVA_HOME}').")
endif()
file(GLOB WMA_JNI_MD_HEADERS "${WMA_JAVA_HOME}/include/*/jni_md.h")
if(NOT WMA_JNI_MD_HEADERS)
    message(FATAL_ERROR "jni.h existe en ${WMA_JAVA_HOME}/include pero no su jni_md.h.")
endif()
list(GET WMA_JNI_MD_HEADERS 0 WMA_JNI_MD_HEADER)
get_filename_component(WMA_JNI_MD_DIR "${WMA_JNI_MD_HEADER}" DIRECTORY)
set(WMA_JNI_INCLUDE_DIRS "${WMA_JAVA_HOME}/include" "${WMA_JNI_MD_DIR}")
message(STATUS "host-JNI: jni.h desde ${WMA_JAVA_HOME}/include (md: ${WMA_JNI_MD_DIR})")

# Mismas banderas que ../CMakeLists.txt y que ios/CMakeLists.txt. Compra algo
# que no tenia ningun gate: los tres archivos de jni/ no pasaban por -Wall/-Werror
# bajo NINGUN compilador (el CMakeLists que maneja AGP no tiene banderas).
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
option(WMA_HOSTJNI_WERROR "Treat host-JNI build warnings as errors" ON)
if(WMA_HOSTJNI_WERROR)
    add_compile_options(-Werror)
endif()

include(${WMA_CPP_ROOT}/thirdparty/tinysoundfont.cmake)
add_subdirectory(${WMA_CPP_ROOT}/dsp      ${CMAKE_BINARY_DIR}/dsp)
add_subdirectory(${WMA_CPP_ROOT}/effects  ${CMAKE_BINARY_DIR}/effects)
add_subdirectory(${WMA_CPP_ROOT}/engines  ${CMAKE_BINARY_DIR}/engines)
add_subdirectory(${WMA_CPP_ROOT}/voice    ${CMAKE_BINARY_DIR}/voice)
add_subdirectory(${WMA_CPP_ROOT}/analysis ${CMAKE_BINARY_DIR}/analysis)
add_subdirectory(${WMA_CPP_ROOT}/looper   ${CMAKE_BINARY_DIR}/looper)

# El motor tal como lo linkea la suite de host (core/tests/CMakeLists.txt), MAS
# la capa JNI. `jni/jni_benchmark.cpp` queda afuera: depende de <oboe/Oboe.h> y
# no cross-compila — medido, y declarado como hueco en spec.md.
add_library(watermelon_audio SHARED
    ${WMA_CPP_ROOT}/jni/jni_audio_bridge.cpp
    ${WMA_CPP_ROOT}/jni/jni_engine.cpp
    ${WMA_CPP_ROOT}/jni/jni_usb.cpp
    ${WMA_CPP_ROOT}/usb/RoundTripMeasurer.cpp
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
    ${WMA_CPP_ROOT}/nodes/InputNode.cpp
    ${WMA_CPP_ROOT}/looper/LooperExporter.cpp
    ${WMA_CPP_ROOT}/platform/Logger.cpp
    ${WMA_CPP_ROOT}/platform/PlatformAndroid.cpp
    ${WMA_CPP_ROOT}/backends/BackendManager.cpp
    ${WMA_CPP_ROOT}/backends/SplitBackend.cpp
    ${WMA_CPP_ROOT}/backends/DriftResampler.cpp
    # La UNICA TU sustituida, la misma que sustituye la suite de host: le da a
    # BackendManager un FakeAudioBackend en vez del nullptr que devuelve el
    # factory real fuera de Android. Ver el 🔴 del encabezado.
    ${WMA_CPP_ROOT}/core/tests/support/test_platform_backends.cpp
    # Los cuerpos vacios de LibusbBackend que jni_usb.cpp nombra. USB necesita
    # dispositivo y queda fuera de alcance (spec.md): esto existe para que el
    # .so cierre sus simbolos, no para probar USB.
    stub_libusb_backend.cpp
)

target_include_directories(watermelon_audio PRIVATE
    ${WMA_CPP_ROOT}
    ${WMA_CPP_ROOT}/core/tests
    ${WMA_CPP_ROOT}/thirdparty/tinysoundfont
    # LibusbBackend.h incluye <libusb.h>. El header alcanza: los cuerpos los da
    # stub_libusb_backend.cpp y no se linkea la libreria.
    ${WMA_CPP_ROOT}/thirdparty/libusb/libusb
    ${WMA_JNI_INCLUDE_DIRS}
)

find_package(Threads REQUIRED)
target_link_libraries(watermelon_audio PRIVATE
    watermelon-analysis
    watermelon-voice
    watermelon-looper
    watermelon-engines
    watermelon-effects
    watermelon-dsp
    tinysoundfont
    Threads::Threads
)

# Un .so acepta simbolos indefinidos por default y falla RECIEN al cargarse — o
# sea, dentro del test, como un `UnsatisfiedLinkError` que parece del arnes.
# Esto lo convierte en un error de LINK, que dice el nombre del simbolo.
# En macOS ya es el default para un dylib (y `-undefined error` esta deprecado);
# en Linux hay que pedirlo.
if(NOT APPLE)
    target_link_options(watermelon_audio PRIVATE "LINKER:--no-undefined")
endif()
