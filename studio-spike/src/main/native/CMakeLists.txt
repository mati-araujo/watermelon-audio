# REQ-055 S3 — nativo de la espiga: el dylib de PRODUCCION del desktop
# (audio/src/main/cpp/desktop) mas un shim JNI propio de la espiga que expone el
# diagnostico de CoreAudio en macOS (backends/CoreAudioMacDiag.h) a la JVM.
#
# El shim vive aca y no en audio/src/main/cpp/jni/ a proposito: no es superficie de
# :audio, no entra a los conteos ni a los gates de la frontera JNI, y no se publica.
cmake_minimum_required(VERSION 3.22.1)
project(studio_spike_native CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "La espiga de S3 es sólo macOS (el shim lee CoreAudioMacDiag).")
endif()

# Las dos dylibs en el mismo directorio: es como viajan en el bundle.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(WMA_CPP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../audio/src/main/cpp)
add_subdirectory(${WMA_CPP_ROOT}/desktop ${CMAKE_BINARY_DIR}/watermelon)

add_library(studio_spike_jni SHARED spike_jni.cpp)
target_compile_options(studio_spike_jni PRIVATE -Wall -Wextra -Werror)
target_include_directories(studio_spike_jni PRIVATE
    ${WMA_CPP_ROOT}/backends ${WMA_JAVA_HOME}/include ${WMA_JAVA_HOME}/include/darwin)
target_link_libraries(studio_spike_jni PRIVATE watermelon_audio)
# Las dos dylibs viajan juntas en el mismo directorio del bundle: el UNICO rpath es
# @loader_path. Sin BUILD_WITH_INSTALL_RPATH, CMake agrega la ruta absoluta del build,
# y un bundle firmado no tiene por que buscar codigo en un directorio de /tmp.
set_target_properties(studio_spike_jni PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path")
