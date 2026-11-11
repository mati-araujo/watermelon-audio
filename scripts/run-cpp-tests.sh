#!/usr/bin/env bash
# ============================================================================
# Build and run the watermelon-audio host C++ test suite (dsp + effects +
# looper + usb) in a single configure / build / ctest pass.
#
# Host x86_64 only — no Android SDK build, no NDK, no Oboe, no device.
# Used by CI (ubuntu) and usable on any Linux/macOS dev box.
#
# Usage:
#   scripts/run-cpp-tests.sh                 # build + run everything
#   scripts/run-cpp-tests.sh -R Clock        # ctest -R filter (extra args pass through)
#   CLEAN=1 scripts/run-cpp-tests.sh         # wipe build dir first (keeps .deps cache)
#   SANITIZE=address,undefined scripts/run-cpp-tests.sh   # ASan+UBSan build
#   SANITIZE=thread scripts/run-cpp-tests.sh              # TSan build
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_ROOT/audio/src/main/cpp/tests"
BUILD_DIR="$SRC_DIR/build"
DEPS_DIR="$SRC_DIR/.deps"

GEN_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GEN_ARGS=(-G Ninja)
fi

# Optional sanitizers: SANITIZE=address,undefined | thread. Instruments the
# whole host-test graph (see tests/CMakeLists.txt). Forces a clean build so the
# flags apply to every object; use a separate BUILD_DIR to keep the normal
# (non-sanitized) build cache intact.
SAN_ARGS=()
if [[ -n "${SANITIZE:-}" ]]; then
    echo "Sanitizers enabled: $SANITIZE"
    SAN_ARGS=(-DWMA_TESTS_SANITIZE="$SANITIZE")
    BUILD_DIR="$SRC_DIR/build-san"
    rm -rf "$BUILD_DIR"
fi

if [[ "${CLEAN:-0}" == "1" ]]; then
    echo "Cleaning $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
fi

# The "${arr[@]+"${arr[@]}"}" dance is required, not decorative: macOS ships
# bash 3.2, where `set -u` treats an empty array as unbound and aborts the
# script. GEN_ARGS is empty whenever ninja is absent, and SAN_ARGS is empty on
# every non-sanitizer run — so a plain "${arr[@]}" breaks every macOS run.
cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${GEN_ARGS[@]+"${GEN_ARGS[@]}"}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DFETCHCONTENT_BASE_DIR="$DEPS_DIR" \
    "${SAN_ARGS[@]+"${SAN_ARGS[@]}"}"

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# LA TRAZA DE ESPERAS CIEGAS (MINI-057). Este script es su DUENO: `sleepFixed` deja un
# archivo vacio por test que la llama en $BUILD_DIR/wait-trace/ (TestWait.h), y
# `check-time-dependence.sh` re-corre solo esos. Un directorio viejo no puede colarse:
# se VACIA antes de ctest, y el MANIFEST se escribe despues y solo si la traza es de
# una corrida COMPLETA:
#   - sin filtro: un -R/-E/-L/-I (o cualquier otra forma de elegir tests) deja tests
#     sin correr, y sus esperas faltan de la traza;
#   - con ctest en verde (rc 0): un test que aborta porque no pudo escribir la traza
#     (AC-057.4) no deja su archivo, asi que una suite roja tiene la traza incompleta;
#   - con el arbol quieto: el sello se toma ANTES del build y se vuelve a tomar despues de
#     ctest, y el MANIFEST solo se escribe si coinciden. Sellar solo al final daria fe de un
#     arbol que pudo cambiar entre lo que se compilo y lo que se midio.
# La traza se vacia antes del build, no solo antes de ctest.
TRACE_DIR="$BUILD_DIR/wait-trace"
rm -rf "$TRACE_DIR"
mkdir -p "$TRACE_DIR"

filtered=0
for arg in "$@"; do
    case "$arg" in
        -R*|-E*|-L*|-I*|-F*|-N*|-U*|--tests-*|--exclude-*|--label-*|--rerun-failed|--fixture-*|--show-only*|--stop-on-failure) filtered=1 ;;
    esac
done

# Sello del arbol: tree-id de HEAD + cambios + archivos nuevos no ignorados, via un
# indice temporal (no toca el real). Cambia si cambia cualquier fuente. IDENTICA a la
# de check-time-dependence.sh; si divergen, el SHA no coincide y el detector corre todo.
tree_stamp() {
    local d idx
    d="$(mktemp -d)" || return 0
    idx="$d/index"
    ( cd "$REPO_ROOT" && export GIT_INDEX_FILE="$idx" \
        && git read-tree HEAD >/dev/null 2>&1 \
        && git add -A . >/dev/null 2>&1 \
        && git write-tree 2>/dev/null ) || true
    rm -rf "$d"
}

stamp_before="$(tree_stamp)"

cmake --build "$BUILD_DIR" -j"$JOBS"

# ctest corria EN SERIE, y era el 85 % del job mas caro del CI: bajo TSan son
# 539 s de ctest contra 95 s de build. Medido en esta suite, con los 883 tests:
# 177 s en serie contra 24,6 s con -j8, y el `user` time casi igual (107 s
# contra 116 s) — o sea paralelismo puro, no un efecto de cache.
#
# Los tests son aislables por construccion: cada uno que escribe usa un nombre
# de archivo propio. Los dos nombres que se repiten (`/tmp/nope.wav`, `x.wav`)
# estan en aserciones de camino negativo que nunca escriben.
#
# CTEST_JOBS lo baja quien lo necesite —un runner con poca RAM bajo TSan, o
# depurar un flake— y un `-j` en "$@" tambien gana, porque va despues.
#
rc=0
WMA_TEST_WAIT_TRACE="$TRACE_DIR" \
    ctest --test-dir "$BUILD_DIR" --output-on-failure -j "${CTEST_JOBS:-$JOBS}" "$@" || rc=$?

if [[ $rc -eq 0 && $filtered -eq 0 ]]; then
    stamp="$(tree_stamp)"
    if [[ -z "$stamp_before" || "$stamp" != "$stamp_before" ]]; then
        echo "wait-trace: el arbol cambio (o no se pudo sellar) entre el build y el fin de ctest; sin MANIFEST (el detector correra todo)" >&2
    else
        total="$(ctest --test-dir "$BUILD_DIR" -N | sed -n 's/^Total Tests: //p')"
        traced="$(find "$TRACE_DIR" -maxdepth 1 -type f | wc -l | tr -d ' ')"
        printf 'tree=%s\ntests=%s\ntraced=%s\n' "$stamp" "$total" "$traced" >"$TRACE_DIR/MANIFEST.tmp"
        mv "$TRACE_DIR/MANIFEST.tmp" "$TRACE_DIR/MANIFEST"
    fi
fi
exit "$rc"
