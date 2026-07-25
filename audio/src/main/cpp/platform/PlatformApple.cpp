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
// flushDenormals() and the SIMD capability flags are ISA-level and shared with
// Android — see PlatformIsa.inc (WA-1.6). Only the scheduler call below is
// genuinely Apple-specific.
#define WMA_PLATFORM_LABEL "Apple"
#include "PlatformIsa.inc"

namespace wma { namespace platform {

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

}} // namespace wma::platform
