# pffft - FFT real con SIMD (licencia FFTPACKv5 de UCAR, tipo BSD)
# https://github.com/marton78/pffft - commit y verificacion en VENDORED.md
#
# Nadie fuera de dsp/RealFFT.cpp la usa: la FFT del motor es `dsp/RealFFT.h` (R-BUILD-3).
# La incluye dsp/CMakeLists.txt, el unico camino que comparten Android, iOS, desktop y la
# suite de host.

# pffft es C y los proyectos del motor son CXX.
enable_language(C)

# CMAKE_CURRENT_LIST_DIR: ver tinysoundfont.cmake.
set(PFFFT_DIR ${CMAKE_CURRENT_LIST_DIR}/pffft)

add_library(pffft STATIC
    ${PFFFT_DIR}/src/pffft.c
    ${PFFFT_DIR}/src/pffft_common.c
)

target_include_directories(pffft PUBLIC ${PFFFT_DIR}/include/pffft PRIVATE ${PFFFT_DIR}/src)

# PUBLIC: sin esto el header declara dllimport en Windows y no compila con -Werror.
target_compile_definitions(pffft PUBLIC PFFFT_STATIC_DEFINE)

# Suppress warnings in third-party code
target_compile_options(pffft PRIVATE -w)
