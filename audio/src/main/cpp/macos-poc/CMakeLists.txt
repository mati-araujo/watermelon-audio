# PoC MINI-028 (NoisyPad para Mac): espiga del motor en macOS.
# Reusa el build de iOS tal cual (ios/CMakeLists.txt) apuntado a macOS arm64 y le
# cuelga un binario de consola que maneja el motor SÓLO por la API C (wma_*),
# la misma frontera que consume el cinterop de iOS. Rama poc/macos-engine: no se mergea.
#
#   cmake -S macos-poc -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
#   cmake --build <build> && <build>/wma-macos-spike [segundos] [frames]
cmake_minimum_required(VERSION 3.22.1)
project(wma_macos_spike CXX OBJCXX)

add_compile_definitions(WMA_POC_RT_STATS=1)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../ios ${CMAKE_BINARY_DIR}/engine)

add_executable(wma-macos-spike main.cpp)
target_include_directories(wma-macos-spike PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
target_link_libraries(wma-macos-spike PRIVATE watermelon-core
    "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio" "-framework Foundation")
