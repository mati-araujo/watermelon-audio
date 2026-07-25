// ============================================================================
// Shared ISA-level implementations of the platform layer (WA-1.6).
//
// Everything in here depends only on the instruction set, not on the operating
// system: denormal flushing (FPCR / FPSCR / MXCSR) and the compile-time SIMD
// capability flags. Anything that talks to an OS scheduler — thread priority,
// in particular — stays in the per-platform .cpp, because that is where the two
// implementations genuinely differ.
//
// WA-1.6 called for a "PlatformArm64.inc", but the duplication turned out to be
// wider than the arm64 block: the x86_64 MXCSR branch and BOTH SIMD capability
// functions were byte-for-byte identical between PlatformAndroid.cpp and
// PlatformApple.cpp too. Hence the broader name and scope.
//
// This is a .inc rather than a header because it DEFINES functions in
// wma::platform. Exactly one translation unit per build may include it, which
// is guaranteed by there being exactly one PlatformXxx.cpp per platform — the
// same one-file-per-platform rule backends/PlatformBackends.cpp follows.
//
// Include it at file scope (not inside a namespace) after defining
// WMA_PLATFORM_LABEL to a string literal. The label only tags log lines, so a
// build can never silently lose track of which implementation it picked up.
// ============================================================================

#ifndef WMA_PLATFORM_LABEL
#error "Define WMA_PLATFORM_LABEL (e.g. \"Android\") before including PlatformIsa.inc"
#endif

#include <cstdint>

namespace wma { namespace platform {

void flushDenormals() {
#if defined(__aarch64__) && defined(USE_NEON)
    // ARM64: set FPCR.FZ (flush-to-zero) and FPCR.DN (default NaN). FPCR is part
    // of the ARM64 ISA, so these are the same instructions on Apple Silicon and
    // on an Android phone — which is the whole reason this block is shared.
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  // FZ bit - flush denormals to zero
    fpcr |= (1ULL << 25);  // DN bit - default NaN mode
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
    WMA_LOGI("ARM64 (" WMA_PLATFORM_LABEL "): Denormal flush enabled "
             "(FPCR.FZ=1, FPCR.DN=1)");

#elif defined(__arm__) && defined(USE_NEON)
    // ARMv7: FPSCR.FZ. Reachable on Android's armeabi-v7a ABI only; Apple has
    // shipped no 32-bit ARM target for years, but the branch costs nothing.
    uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1 << 24);  // FZ bit
    asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
    WMA_LOGI("ARMv7 (" WMA_PLATFORM_LABEL "): Denormal flush enabled "
             "(FPSCR.FZ=1)");

#elif defined(__x86_64__) || defined(_M_X64)
    // x86_64: MXCSR FZ (bit 15) + DAZ (bit 6). Android emulator ABIs, Intel
    // Macs, and the host test suite all land here.
    unsigned int mxcsr;
    asm volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr |= (1 << 15);  // FZ - Flush to Zero
    mxcsr |= (1 << 6);   // DAZ - Denormals Are Zero
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    WMA_LOGI("x86_64 (" WMA_PLATFORM_LABEL "): Denormal flush enabled "
             "(MXCSR.FZ=1, MXCSR.DAZ=1)");

#else
    WMA_LOGW("No denormal flush available for this architecture "
             "(" WMA_PLATFORM_LABEL ")");
#endif
}

bool hasNeonSupport() {
#if defined(USE_NEON)
    return true;
#else
    return false;
#endif
}

bool hasSseSupport() {
#if defined(USE_SSE)
    return true;
#else
    return false;
#endif
}

}} // namespace wma::platform
