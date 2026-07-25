#!/usr/bin/env bash
# ============================================================================
# Build the portable C++ core for iOS — device (arm64) and simulator (arm64).
# WA-2.1.
#
# Produces one merged static archive per slice:
#   audio/src/main/cpp/ios/build/iphoneos/libwatermelon_audio.a
#   audio/src/main/cpp/ios/build/iphonesimulator/libwatermelon_audio.a
#
# SCOPE: the whole engine — core/, nodes/, api/, backends/, platform/ and the
# portable DSP sub-libraries. See audio/src/main/cpp/ios/CMakeLists.txt for what
# is excluded and why.
#
# Each slice is then link-checked: a trivial main() is linked against the merged
# archive with -force_load, which pulls in EVERY member and so forces every
# symbol any member references to resolve. That is the check that matters for
# WA-3.1 — a STATIC target happily builds with dangling references, and `nm -u`
# can only guess at which undefined symbols the system will provide.
#
# Usage:
#   scripts/build-ios.sh                  # both slices, Debug
#   scripts/build-ios.sh Release          # both slices, Release
#   CLEAN=1 scripts/build-ios.sh          # wipe the build dirs first
#
# Requires: Xcode (not just the Command Line Tools) and cmake.
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IOS_DIR="$REPO_ROOT/audio/src/main/cpp/ios"
BUILD_ROOT="$IOS_DIR/build"
BUILD_TYPE="${1:-Debug}"
DEPLOYMENT_TARGET="15.0"

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found (brew install cmake)" >&2
    exit 1
fi
if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    echo "error: iOS SDK not available. Install Xcode and run:" >&2
    echo "         sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
    echo "         sudo xcodebuild -runFirstLaunch" >&2
    exit 1
fi

# Empty arrays under `set -u` abort on macOS's bash 3.2, hence the
# "${arr[@]+...}" form — see scripts/run-cpp-tests.sh for the full story.
GEN_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GEN_ARGS=(-G Ninja)
fi

if [[ "${CLEAN:-0}" == "1" ]]; then
    echo "Cleaning $BUILD_ROOT ..."
    rm -rf "$BUILD_ROOT"
fi

for SDK in iphoneos iphonesimulator; do
    BUILD_DIR="$BUILD_ROOT/$SDK"
    echo
    echo "=== slice: $SDK ($BUILD_TYPE) ==="

    cmake -S "$IOS_DIR" -B "$BUILD_DIR" "${GEN_ARGS[@]+"${GEN_ARGS[@]}"}" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT="$SDK" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

    # -k 0 keeps going after the first failure: ninja stops at one error by
    # default, which hides the rest of a portability break behind whichever
    # file happened to fail first.
    cmake --build "$BUILD_DIR" -- -k 0

    # CMake will not merge static archives; libtool does.
    MERGED="$BUILD_DIR/libwatermelon_audio.a"
    # shellcheck disable=SC2046  # word splitting is what we want here
    libtool -static -o "$MERGED" $(find "$BUILD_DIR" -name '*.a' ! -name 'libwatermelon_audio.a')

    echo "  -> $MERGED"
    echo "     archs:   $(lipo -archs "$MERGED")"
    echo "     size:    $(du -h "$MERGED" | cut -f1)"
    echo "     symbols: $(nm -gU "$MERGED" 2>/dev/null | grep -c ' T ' || true)"

    # --- link check -------------------------------------------------------
    # The simulator triple needs the -simulator suffix; without it clang builds
    # for a device and the sysroot mismatches.
    if [[ "$SDK" == "iphonesimulator" ]]; then
        TRIPLE="arm64-apple-ios${DEPLOYMENT_TARGET}-simulator"
    else
        TRIPLE="arm64-apple-ios${DEPLOYMENT_TARGET}"
    fi

    PROBE_SRC="$BUILD_DIR/link_probe.cpp"
    echo 'int main(void) { return 0; }' > "$PROBE_SRC"

    echo "  link check ($TRIPLE) ..."
    xcrun --sdk "$SDK" clang++ -std=c++20 \
        -target "$TRIPLE" \
        -isysroot "$(xcrun --sdk "$SDK" --show-sdk-path)" \
        "$PROBE_SRC" \
        -Wl,-force_load,"$MERGED" \
        -framework AVFoundation \
        -framework AudioToolbox \
        -framework CoreAudio \
        -framework Foundation \
        -o "$BUILD_DIR/link_probe"
    echo "     OK — every symbol in the archive resolves."
done

echo
echo "OK — both slices built and link-checked."
