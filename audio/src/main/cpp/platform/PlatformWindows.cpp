#include "Platform.h"

#undef WMA_LOG_TAG
#define WMA_LOG_TAG "WmaPlatform"
#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOGDI: wingdi.h defines ERROR, which is wma::LogLevel::ERROR in Logger.h.
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>

// Windows implementation of the platform layer (REQ-061 S2).
//
// flushDenormals() and the SIMD capability flags are ISA-level and shared with Android and Apple —
// see PlatformIsa.inc (WA-1.6). Only the scheduler call below is genuinely Windows-specific.
#define WMA_PLATFORM_LABEL "Windows"
#include "PlatformIsa.inc"

namespace wma { namespace platform {

void setAudioThreadPriority() {
    // Best-effort supplement, like the Android and Apple ones: the WASAPI backend (REQ-062) registers
    // its render thread with MMCSS itself. TIME_CRITICAL is the top of the ordinary priority classes:
    // it needs no privilege and cannot starve the system the way a realtime-class process could.
    if (::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        WMA_LOGI("Audio thread priority set to THREAD_PRIORITY_TIME_CRITICAL");
    } else {
        WMA_LOGW("Could not set THREAD_PRIORITY_TIME_CRITICAL (error=%lu); leaving the thread priority alone",
                 static_cast<unsigned long>(::GetLastError()));
    }
}

int32_t currentKernelThreadId() {
    // The ADPF and sched_setaffinity users of this id are Android/Linux; Windows has no kernel tid
    // they accept.
    return -1;
}

}} // namespace wma::platform
