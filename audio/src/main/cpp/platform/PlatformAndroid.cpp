#include "Platform.h"

#undef WMA_LOG_TAG
#define WMA_LOG_TAG "WmaPlatform"
#include "Logger.h"

#include <sched.h>
#include <unistd.h>
#include <sys/resource.h>

// Android implementation of the platform layer.
//
// flushDenormals() and the SIMD capability flags are ISA-level and shared with
// Apple — see PlatformIsa.inc (WA-1.6). Only the scheduler call below is
// genuinely Android-specific.
#define WMA_PLATFORM_LABEL "Android"
#include "PlatformIsa.inc"

namespace wma { namespace platform {

void setAudioThreadPriority() {
#if defined(__ANDROID__)
    // Android: set SCHED_FIFO with high priority for audio threads.
    // Note: Oboe typically handles this, but we provide it for custom backends.
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        // Fallback: at least try to increase nice value
        setpriority(PRIO_PROCESS, 0, -19);
        WMA_LOGW("Could not set SCHED_FIFO, using high nice priority");
    } else {
        WMA_LOGI("Audio thread priority set to SCHED_FIFO");
    }
#endif
}

}} // namespace wma::platform
