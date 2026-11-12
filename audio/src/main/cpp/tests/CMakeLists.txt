# ============================================================================
# watermelon-audio — unified host test suite
#
# Aggregates every host-side (x86_64) Google Test suite into ONE configure /
# build / ctest run, sharing a single googletest download. Runs on the dev
# host (Windows/Linux/macOS) — NOT on an Android device. No Oboe, no JNI,
# no NDK required.
#
#   dsp_tests      — audio/src/main/cpp/dsp/tests
#   effects_tests  — audio/src/main/cpp/effects/tests
#   looper_tests   — audio/src/main/cpp/looper/tests
#   usb_tests      — audio/src/main/cpp/usb/tests
#   libusb_descriptor_tests — audio/src/main/cpp/usb/tests/libusb (REQ-047 S1:
#                    el nucleo de libusb vendorizado sobre un backend falso;
#                    solo se configura desde aca, no standalone)
#   core_tests     — audio/src/main/cpp/core/tests
#   core_cost_tests — audio/src/main/cpp/core/tests (MINI-057 D20: el test de costo
#                    pareado de la compuerta, en su propio ejecutable, todo a -O0)
#   analysis_tests — audio/src/main/cpp/analysis/tests
#
# Prefer the wrapper scripts, which locate a host toolchain (MinGW g++ +
# Ninja) for you:
#
#   pwsh   scripts/run-cpp-tests.ps1      (Windows)
#   bash   scripts/run-cpp-tests.sh       (Linux / CI / macOS)
#
# Manual equivalent:
#   cmake -S audio/src/main/cpp/tests -B audio/src/main/cpp/tests/build -G Ninja
#   cmake --build audio/src/main/cpp/tests/build
#   ctest --test-dir audio/src/main/cpp/tests/build --output-on-failure
#
# Each child CMakeLists still builds standalone (cd into it + cmake) — the
# `if(NOT TARGET ...)` guards added to dsp/ and effects/ make the shared
# library subdirectories safe to pull in more than once from here.
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(watermelon-cpp-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

# ----------------------------------------------------------------------------
# Descubrimiento de tests en tiempo de TEST, no de BUILD.
#
# `gtest_discover_tests()` por defecto usa POST_BUILD: apenas linkea un binario
# lo EJECUTA con --gtest_list_tests y parsea su salida. Si esa invocación no
# devuelve nada —el binario no arranca, o el runner se queda sin recursos con
# ocho descubrimientos en paralelo— CMake no lo reporta como "no pude listar":
# lo reporta como ERROR DE BUILD, con un `string sub-command JSON failed
# parsing json string` que no menciona ningún test.
#
# Eso ya pasó dos veces en el job de macOS (2026-07-27 y 2026-07-28), las dos
# sobre `looper_tests`/`looper_tests_dense`, en PRs que no tocaban una línea de
# C++. Es la mitad de todas las fallas históricas de ese job.
#
# PRE_TEST mueve el listado a tiempo de `ctest`, así que el build deja de
# depender de ejecutar binarios. Una falla real de arranque sigue apareciendo
# —como falla de TEST, que es lo que es— en vez de disfrazarse de build roto.
#
# Es el mismo eslabón que ya tenía documentado su otra forma: con
# `detect_leaks=1` en macOS el discovery aborta y el build falla antes de
# correr un solo test (ver CLAUDE.md).
#
# OJO: se setea acá, en el raíz, y hereda a los ocho `gtest_discover_tests()`
# de los subdirectorios. Un subdirectorio configurado STANDALONE (que sigue
# siendo posible, ver el encabezado) no lo hereda y vuelve a POST_BUILD.
# ----------------------------------------------------------------------------
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

# ----------------------------------------------------------------------------
# Sanitizers — host test build ONLY (never the NDK/production build). Enable
# with -DWMA_TESTS_SANITIZE=address,undefined  or  =thread. Placed BEFORE the
# googletest fetch so the whole graph (gtest + the watermelon libs + the tests)
# is instrumented — TSan in particular needs every translation unit compiled
# with -fsanitize=thread. See the two CI jobs in .github/workflows/ci.yml.
# ----------------------------------------------------------------------------
set(WMA_TESTS_SANITIZE "" CACHE STRING
    "Host-test sanitizers passed to -fsanitize, e.g. 'address,undefined' or 'thread'")
if(WMA_TESTS_SANITIZE)
    message(STATUS "Host tests: sanitizers = ${WMA_TESTS_SANITIZE}")
    add_compile_options(-fsanitize=${WMA_TESTS_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${WMA_TESTS_SANITIZE})
endif()

# ----------------------------------------------------------------------------
# Optimizacion de la suite — host test build ONLY. Un solo punto para la suite
# normal, ASan/UBSan, TSan y macOS (MINI-057 D5).
#
# -O1 -g, con CMAKE_BUILD_TYPE=Debug conservado: NDEBUG sigue sin definirse y los
# assert() siguen vivos. A -O0 los tests que miden DSP pagaban entre 4 y 12 veces
# lo que cuestan optimizados. -O1 es el nivel que recomiendan ASan y TSan.
# -DWMA_TESTS_OPT=-O0 sirve para depurar.
#
# add_compile_options va DESPUES de CMAKE_CXX_FLAGS y CMAKE_CXX_FLAGS_DEBUG en la
# linea de compilacion, asi que este -O gana sobre cualquiera que traiga el tipo
# de build (GCC y clang: el ultimo -O manda).
# ----------------------------------------------------------------------------
# Excepcion declarada (MINI-057 D6): TSan compila a -O0. Medido en CI (run 42942604293,
# GCC 15.2.0, 5 corridas): a -O1 GCC expande en linea el memcpy del payload del anillo SPSC
# y esos accesos no llegan a TSan; una carrera sembrada en LockFreeRingBuffer da 0/5 a -O1
# y 5/5 a -O0. ASan y UBSan sí ven sus mutaciones 5/5 a -O1 y se quedan en -O1.
# Excepcion declarada (MINI-057 D29): Windows (MinGW) tambien compila a -O0, identico a master.
# La suite de Windows no se puede verificar desde la maquina de desarrollo (App Control bloquea
# los .exe locales) y a -O1 se activarian skips por __OPTIMIZE__ (tests/windows-not-run.txt,
# check-windows-not-run --results) y techos de tiempo de pared que nadie midio en Windows. El
# ahorro de tiempo en Windows no es objetivo del MINI. WIN32 vale con el toolchain cruzado
# (CMAKE_SYSTEM_NAME Windows, windows-cross) y con el configure nativo de run-cpp-tests.ps1 en MSYS2.
# Excepcion declarada (MINI-057 D23): Apple clang (macOS) tambien compila a -O0, identico a master.
# A -O1 cambia el veredicto de SongMix.ABurstOfMixSettersAgainstTheCallbackAllocatesNothingAndStaysInsideThePeriod:
# run final 43206186559, intento 5, job 131204207295 (`Host tests: optimization = -O1`):
# `Expected: (static_cast<double>(costs.back())) < (kPeriodNs), actual: 10902875 vs 5333333.333333333`
# (un callback tardo mas que su periodo). En master (macOS -O0) esa asercion del maximo no se
# evalua (va bajo __OPTIMIZE__), asi que a -O1 es un veredicto nuevo sin evidencia previa (D21).
# Este es el punto unico: lo heredan todos los configures (sh, ps1, CI, gate.sh).
# WMA_TESTS_OPT vacio = automatico; un valor explicito siempre manda.
set(WMA_TESTS_OPT "" CACHE STRING
    "Optimization flag for the host test suite; empty = auto (-O1; -O0 under TSan, on Windows and on Apple clang)")
if(WMA_TESTS_OPT)
    set(WMA_TESTS_OPT_EFFECTIVE "${WMA_TESTS_OPT}")
elseif(WMA_TESTS_SANITIZE MATCHES "thread")
    set(WMA_TESTS_OPT_EFFECTIVE "-O0")
elseif(WIN32 OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(WMA_TESTS_OPT_EFFECTIVE "-O0")
else()
    set(WMA_TESTS_OPT_EFFECTIVE "-O1")
endif()
message(STATUS "Host tests: optimization = ${WMA_TESTS_OPT_EFFECTIVE}")
add_compile_options(${WMA_TESTS_OPT_EFFECTIVE} -g)

# Fetch googletest once, here. The child suites include the same file so they
# still configure standalone, but include_guard(GLOBAL) makes this the only
# fetch. The version lives ONLY in thirdparty/googletest.cmake (REQ-047 S4);
# scripts/check-dep-pins.py fails if a second pin shows up anywhere.
include(${CMAKE_CURRENT_LIST_DIR}/../thirdparty/googletest.cmake)

# ----------------------------------------------------------------------------
# Warning gate — host test build ONLY (never the NDK/production build).
#
# Placed AFTER googletest is fetched so third-party code is not held to it, and
# BEFORE the test suites so it propagates to them and to the watermelon-* libs
# they compile. This is what catches real defects early (e.g. the uninitialized
# `a0` in FilterEffect). Turn the gate off locally with -DWMA_TESTS_WERROR=OFF.
# ----------------------------------------------------------------------------
# -Wno-unused-parameter: this engine has many interface defaults / override
# stubs that legitimately ignore parameters; flagging them is noise. The
# high-value checks (uninitialized, reorder, format, unused-variable) stay on.
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

if(WIN32)
    # MinGW's default (msvcrt) printf rejects C99 %zu/%lld length modifiers that
    # are valid on the Android/bionic target, producing host-only false -Wformat
    # positives. This switches MinGW to its C99-compliant stdio so -Wformat
    # validates against the real target semantics instead of blocking the gate.
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

option(WMA_TESTS_WERROR "Treat host-test warnings as errors" ON)
if(WMA_TESTS_WERROR)
    add_compile_options(-Werror)
endif()

# TSan and standalone fences. GCC emits -Wtsan ("'atomic_thread_fence' is not
# supported with '-fsanitize=thread'") for every TU that even *includes*
# TrackBuffer.h, so with the -Werror gate above the thread job died at compile
# time — before running a single test. Demoted here, after the gate, so the
# suite builds.
#
# Be clear about what this buys and what it doesn't: TSan does not model
# standalone fences, so it cannot prove the accesses ordered by the 8 fences in
# looper/TrackBuffer.h (+1 in LooperExporter.cpp) are safe, and it may report
# them as races in the two concurrent looper suites. Nothing outside looper/
# uses fences, so the dsp/usb ring-buffer concurrency — the reason this job
# exists — is fully modelled. Silencing the warning hides a tooling limitation,
# not a defect TSan found.
if(WMA_TESTS_SANITIZE MATCHES "thread")
    # -Wno-tsan is a GCC spelling; Apple clang rejects it as an unknown warning
    # option (fatal under -Werror), which blocks running this job locally on
    # macOS. Guard it to GCC — same rationale as the Clang-only guard on
    # -Wno-unused-private-field in core/tests.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-tsan)
    endif()
endif()

# Order is irrelevant — the shared-library subdirectories are guarded.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../dsp/tests     dsp_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../effects/tests effects_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../engines/tests engine_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../voice/tests   voice_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../looper/tests  looper_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../usb/tests     usb_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../usb/tests/libusb libusb_descriptor_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core/tests    core_tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../analysis/tests analysis_tests)
