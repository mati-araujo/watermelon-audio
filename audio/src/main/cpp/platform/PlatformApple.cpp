#include "Platform.h"

#undef WMA_LOG_TAG
#define WMA_LOG_TAG "WmaPlatform"
#include "Logger.h"

#include <pthread.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

// Apple (iOS / macOS) implementation of the platform layer (WA-2.2).
//
// The arm64 FPCR denormal-flush block below is byte-for-byte the one in
// PlatformAndroid.cpp: FPCR is part of the ARM64 ISA, not anything
// Android-specific, so Apple Silicon uses the exact same instructions. Factoring
// the shared block into a PlatformArm64.inc is WA-1.6; kept inline here to keep
// this change self-contained.

namespace wma { namespace platform {

void flushDenormals() {
#if defined(__aarch64__) && defined(USE_NEON)
    // ARM64: Set FPCR.FZ (Flush-to-Zero) and FPCR.DN (Default NaN) bits.
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  // FZ bit - flush denormals to zero
    fpcr |= (1ULL << 25);  // DN bit - default NaN mode
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
    WMA_LOGI("ARM64 (Apple): Denormal flush enabled (FPCR.FZ=1, FPCR.DN=1)");

#elif defined(__x86_64__)
    // Intel Macs / simulator on Intel hosts: MXCSR FZ (bit 15) + DAZ (bit 6).
    unsigned int mxcsr;
    asm volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr |= (1 << 15);  // FZ - Flush to Zero
    mxcsr |= (1 << 6);   // DAZ - Denormals Are Zero
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    WMA_LOGI("x86_64 (Apple): Denormal flush enabled (MXCSR.FZ=1, MXCSR.DAZ=1)");

#else
    WMA_LOGW("No denormal flush available for this architecture");
#endif
}

void setAudioThreadPriority() {
#if defined(__APPLE__)
    // Core Audio already runs its render thread at real-time priority, so this
    // is a reinforcement, not the primary mechanism. Request a time-constraint
    // policy sized for a typical audio buffer; on failure we simply leave the
    // thread as Core Audio configured it. Mirrors PlatformAndroid, which is
    // likewise a best-effort supplement to Oboe's own prioritisation.
    //
    // Values are in Mach absolute-time units. We do not convert precisely (that
    // needs mach_timebase_info); the fractions below are the Apple-documented
    // defaults for an audio-render workload and are robust across devices.
    thread_time_constraint_policy_data_t policy;
    policy.period      = 0;           // 0 = every scheduling quantum
    policy.computation = 5000;        // ~ expected compute per quantum
    policy.constraint  = 10000;       // hard deadline
    policy.preemptible = 1;

    kern_return_t rc = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    if (rc != KERN_SUCCESS) {
        WMA_LOGW("Could not set THREAD_TIME_CONSTRAINT_POLICY (rc=%d); "
                 "leaving Core Audio's own priority in place", rc);
    } else {
        WMA_LOGI("Audio thread priority set to THREAD_TIME_CONSTRAINT_POLICY");
    }
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
