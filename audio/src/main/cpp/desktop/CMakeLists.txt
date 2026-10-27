# ============================================================================
# watermelon-audio — build de PRODUCCION del desktop (REQ-055 S3, ADR-0009 §3)
#
# Produce `libwatermelon_audio.{dylib,so}`: el motor entero + la capa JNI, para
# que una JVM de escritorio (Compose Desktop) lo cargue por `AudioNativeBridge`
# (jvmSharedMain). Es el tercer CMake del motor y esta SEPARADO de los otros dos
# por la misma razon que ios/CMakeLists.txt: el de AGP es Android de punta a
# punta, y el de tests/hostjni/ lleva FakeAudioBackend y stubs de USB.
#
# 🔴 LO QUE ESTE BUILD NO LLEVA, y es el punto:
#   - ningun fake ni stub (nada de core/tests/, ni host_test_hooks, ni
#     stub_libusb_backend): el backend es el real del sistema;
#   - nada de USB: ni usb/, ni LibusbBackend, ni jni_usb.cpp. Las JNIEXPORT de
#     USB de jni_audio_bridge.cpp quedan afuera con WMA_JNI_USB=0 (D4: USB es
#     Android-only). Llamarlas desde la JVM da UnsatisfiedLinkError;
#   - ni jni_benchmark.cpp (depende de <oboe/Oboe.h>).
#
# Backend por plataforma (el unico punto que nombra backends sigue siendo
# backends/PlatformBackends.cpp):
#   - macOS: CoreAudioBackend (AVAudioEngine + HAL), PlatformApple.cpp;
#   - Linux/otros: backend null (createSystemAudioBackend() devuelve nullptr),
#     PlatformAndroid.cpp, que es POSIX salvo lo que guarda __ANDROID__. D8: se
#     prepara, no se prueba.
#
#   cmake -S audio/src/main/cpp/desktop -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build <build>
# ============================================================================
cmake_minimum_required(VERSION 3.22.1)
if(APPLE)
    project(watermelon_audio_desktop CXX OBJCXX)
else()
    project(watermelon_audio_desktop CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Las sub-librerias son estaticas y terminan dentro de una libreria compartida.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(WMA_CPP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# El jni.h del JDK, buscado igual que en tests/hostjni/CMakeLists.txt (ahi esta el
# porque de no usar find_package(JNI): esta libreria la carga una JVM, nunca linkea libjvm).
set(WMA_JAVA_HOME "$ENV{JAVA_HOME}" CACHE PATH "JDK del que salen jni.h y jni_md.h")
if(NOT WMA_JAVA_HOME OR NOT EXISTS "${WMA_JAVA_HOME}/include/jni.h")
    message(FATAL_ERROR "No encuentro jni.h. Pasa -DWMA_JAVA_HOME=<jdk> o exporta JAVA_HOME "
                        "(observado: '${WMA_JAVA_HOME}').")
endif()
file(GLOB WMA_JNI_MD_HEADERS "${WMA_JAVA_HOME}/include/*/jni_md.h")
if(NOT WMA_JNI_MD_HEADERS)
    message(FATAL_ERROR "jni.h existe en ${WMA_JAVA_HOME}/include pero no su jni_md.h.")
endif()
list(GET WMA_JNI_MD_HEADERS 0 WMA_JNI_MD_HEADER)
get_filename_component(WMA_JNI_MD_DIR "${WMA_JNI_MD_HEADER}" DIRECTORY)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    add_compile_definitions(USE_NEON=1)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Las mismas que ios/CMakeLists.txt (y Android), sin -ffinite-math-only por la
    # misma razon: deja al compilador borrar las guardas isnan/isinf de EffectChain.
    add_compile_options(-O3 -DNDEBUG
        -fno-math-errno -fno-trapping-math -fno-signed-zeros
        -freciprocal-math -fassociative-math -ftree-vectorize)
endif()

include(${WMA_CPP_ROOT}/thirdparty/tinysoundfont.cmake)

# Gate de warnings, despues del third-party y antes de lo propio (como en iOS).
option(WMA_DESKTOP_WERROR "Treat desktop build warnings as errors" ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(WMA_DESKTOP_WERROR)
    add_compile_options(-Werror)
endif()

add_subdirectory(${WMA_CPP_ROOT}/dsp      ${CMAKE_BINARY_DIR}/dsp)
add_subdirectory(${WMA_CPP_ROOT}/effects  ${CMAKE_BINARY_DIR}/effects)
add_subdirectory(${WMA_CPP_ROOT}/engines  ${CMAKE_BINARY_DIR}/engines)
add_subdirectory(${WMA_CPP_ROOT}/looper   ${CMAKE_BINARY_DIR}/looper)
add_subdirectory(${WMA_CPP_ROOT}/voice    ${CMAKE_BINARY_DIR}/voice)
add_subdirectory(${WMA_CPP_ROOT}/analysis ${CMAKE_BINARY_DIR}/analysis)

set(WMA_DESKTOP_SOURCES
    ${WMA_CPP_ROOT}/jni/jni_audio_bridge.cpp
    ${WMA_CPP_ROOT}/jni/jni_engine.cpp
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
    ${WMA_CPP_ROOT}/backends/BackendManager.cpp
    ${WMA_CPP_ROOT}/backends/PlatformBackends.cpp
    ${WMA_CPP_ROOT}/backends/SplitBackend.cpp
    ${WMA_CPP_ROOT}/backends/DriftResampler.cpp
)
if(APPLE)
    list(APPEND WMA_DESKTOP_SOURCES
        ${WMA_CPP_ROOT}/platform/PlatformApple.cpp
        ${WMA_CPP_ROOT}/backends/CoreAudioBackend.mm)
    set_source_files_properties(${WMA_CPP_ROOT}/backends/CoreAudioBackend.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
    list(APPEND WMA_DESKTOP_SOURCES ${WMA_CPP_ROOT}/platform/PlatformAndroid.cpp)
endif()

add_library(watermelon_audio SHARED ${WMA_DESKTOP_SOURCES})
target_compile_definitions(watermelon_audio PRIVATE WMA_JNI_USB=0)
target_include_directories(watermelon_audio PRIVATE
    ${WMA_CPP_ROOT}
    ${WMA_CPP_ROOT}/thirdparty/tinysoundfont
    ${WMA_JAVA_HOME}/include
    ${WMA_JNI_MD_DIR}
)

find_package(Threads REQUIRED)
target_link_libraries(watermelon_audio PRIVATE
    watermelon-analysis watermelon-voice watermelon-looper watermelon-engines
    watermelon-effects watermelon-dsp tinysoundfont Threads::Threads)

if(APPLE)
    target_link_libraries(watermelon_audio PRIVATE
        "-framework AVFoundation" "-framework AudioToolbox"
        "-framework CoreAudio" "-framework Foundation")
    # La carga es por ruta absoluta dentro del bundle (ADR-0009 §4). El install name
    # queda `@rpath/libwatermelon_audio.dylib` (default de CMake en macOS): otra dylib
    # del mismo directorio la resuelve con un rpath `@loader_path`, sin rutas del build.
else()
    # Un simbolo indefinido tiene que ser error de LINK, no un UnsatisfiedLinkError
    # al cargar (mismo criterio que tests/hostjni/).
    target_link_options(watermelon_audio PRIVATE "LINKER:--no-undefined")
endif()
