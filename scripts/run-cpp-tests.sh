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

cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${GEN_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DFETCHCONTENT_BASE_DIR="$DEPS_DIR" \
    "${SAN_ARGS[@]}"

cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

ctest --test-dir "$BUILD_DIR" --output-on-failure "$@"
