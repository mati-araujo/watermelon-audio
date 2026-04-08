#include "Platform.h"

#undef WMA_LOG_TAG
#define WMA_LOG_TAG "WmaPlatform"
#include "Logger.h"

#include <sched.h>
#include <unistd.h>
#include <sys/resource.h>

namespace wma { namespace platform {

void flushDenormals() {
#if defined(__aarch64__) && defined(USE_NEON)
    // ARM64: Set FPCR.FZ (Flush-to-Zero) and FPCR.DN (Default NaN) bits
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  // FZ bit - flush denormals to zero
    fpcr |= (1ULL << 25);  // DN bit - default NaN mode
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
    WMA_LOGI("ARM64: Denormal flush enabled (FPCR.FZ=1, FPCR.DN=1)");

#elif defined(__arm__) && defined(USE_NEON)
    // ARMv7: Set FPSCR.FZ bit
    uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1 << 24);  // FZ bit
    asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
    WMA_LOGI("ARMv7: Denormal flush enabled (FPSCR.FZ=1)");

#elif defined(__x86_64__) || defined(_M_X64)
    // x86_64: Set MXCSR FZ (bit 15) and DAZ (bit 6)
    unsigned int mxcsr;
    asm volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr |= (1 << 15);  // FZ - Flush to Zero
    mxcsr |= (1 << 6);   // DAZ - Denormals Are Zero
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    WMA_LOGI("x86_64: Denormal flush enabled (MXCSR.FZ=1, MXCSR.DAZ=1)");

#else
    WMA_LOGW("No denormal flush available for this architecture");
#endif
}

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
