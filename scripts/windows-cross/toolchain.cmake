# Toolchain de CMake del cruce a Windows x64 (MinGW-w64, UCRT). Lo usa check.py, que valida
# el GCC de ese prefijo contra .github/toolchain-pins.json. El prefijo llega por ENTORNO y no
# por -D: los try_compile de CMake releen este archivo sin las variables de la linea de comandos.
if(NOT WMA_CROSS_PREFIX AND DEFINED ENV{WMA_CROSS_PREFIX})
    set(WMA_CROSS_PREFIX "$ENV{WMA_CROSS_PREFIX}")
endif()
if(NOT WMA_CROSS_PREFIX)
    message(FATAL_ERROR "WMA_CROSS_PREFIX sin definir (lo exporta scripts/windows-cross/check.py)")
endif()
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   ${WMA_CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${WMA_CROSS_PREFIX}g++)
set(CMAKE_RC_COMPILER  ${WMA_CROSS_PREFIX}windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
