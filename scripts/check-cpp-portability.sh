#!/usr/bin/env bash
# ============================================================================
# WA-0.4 — portability guardrail for the C++ engine.
#
# Fails if a source file pulls in <jni.h> or <android/...> outside the layers
# that are Android-only by design. Those two headers are the cheapest possible
# proxy for "this file cannot cross-compile for iOS": everything else in the
# engine is meant to build for both platforms, and the iOS build only finds a
# violation once it reaches that file — which, with ninja stopping at the first
# error, can be many PRs later.
#
# The Kotlin half of this guardrail is not a script: it is the `ios` CI job
# compiling both iOS targets on every PR. That is what would have caught the 34
# JVM dependencies WA-0.2 had to clean out of commonMain.
#
# Usage:
#   scripts/check-cpp-portability.sh          # exit 1 on violation
#
# To add an exception, extend is_allowed() below WITH A COMMENT saying why the
# file is Android-only, or why the include is safely guarded. An exception
# without a reason is a hole, not a rule.
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CPP_ROOT="$REPO_ROOT/audio/src/main/cpp"

if [[ ! -d "$CPP_ROOT" ]]; then
    echo "error: C++ root not found at $CPP_ROOT" >&2
    exit 2
fi

# Paths are relative to $CPP_ROOT.
is_allowed() {
    case "$1" in
        # The JNI layer IS the Android bridge — jni.h is its whole reason to exist.
        jni/*) return 0 ;;
        # Oboe and libusb backends are Android-only by decision D4 / platform.
        backends/OboeBackend.*|backends/LibusbBackend.*) return 0 ;;
        # The USB audio driver is Android-only (D4): iOS has no generic USB access.
        usb/*) return 0 ;;
        # The Android implementation of the platform abstraction, by definition.
        platform/PlatformAndroid.cpp) return 0 ;;
        # Shared logger: the <android/log.h> include sits inside a
        # `#if defined(__ANDROID__)` block, so this file compiles for iOS too.
        # It is in the shipped iOS target — see ios/CMakeLists.txt.
        platform/Logger.cpp) return 0 ;;
        *) return 1 ;;
    esac
}

# Vendored code we do not control, and build outputs.
#   thirdparty/  — libusb ships Android examples and an android/log.h include
#   .deps/       — googletest, fetched at configure time
#   build/ .cxx/ — CMake and AGP output trees
FILES=$(cd "$CPP_ROOT" && find . \
    \( -name thirdparty -o -name .deps -o -name build -o -name .cxx \) -prune -o \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \
       -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.mm' \) -print \
    | sed 's|^\./||' | sort)

# Empty arrays under `set -u` abort on macOS's bash 3.2, so violations are
# accumulated in a plain newline-separated string instead of an array.
VIOLATIONS=""
SCANNED=0

FORBIDDEN='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](jni\.h|android/)'

for f in $FILES; do
    SCANNED=$((SCANNED + 1))
    if is_allowed "$f"; then
        continue
    fi
    HITS=$(grep -nE "$FORBIDDEN" "$CPP_ROOT/$f" || true)
    if [[ -n "$HITS" ]]; then
        while IFS= read -r hit; do
            VIOLATIONS="${VIOLATIONS}${f}:${hit}"$'\n'
        done <<< "$HITS"
    fi
done

echo "WA-0.4 portability guardrail: scanned $SCANNED files under audio/src/main/cpp"

if [[ -n "$VIOLATIONS" ]]; then
    echo >&2
    echo "FAIL — Android-only headers outside the Android-only layers:" >&2
    echo >&2
    printf '%s' "$VIOLATIONS" | sed 's/^/  /' >&2
    echo >&2
    echo "These files are part of the portable engine and must cross-compile for" >&2
    echo "iOS. Either move the Android-specific code behind IAudioBackend /" >&2
    echo "wma::platform, guard it with '#if defined(__ANDROID__)', or — if the file" >&2
    echo "is genuinely Android-only — add it to is_allowed() in this script with a" >&2
    echo "comment explaining why." >&2
    exit 1
fi

echo "OK — no forbidden includes."
