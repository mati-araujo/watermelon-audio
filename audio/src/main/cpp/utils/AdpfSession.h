#pragma once

/**
 * @file AdpfSession.h
 * @brief Android Dynamic Performance Framework (ADPF) hint session wrapper.
 *
 * Registers a set of threads that perform short, periodic, deadline-bound work
 * (the USB DSP loop and, optionally, the libusb event loop) with an
 * `APerformanceHintSession`. The framework uses the reported work durations vs.
 * the target to keep the CPU governor from dropping the core frequency (DVFS)
 * during the tiny periodic loads that LOW_LATENCY produces — the dominant cause
 * of the residual scheduling tail (>8 ms reap stalls) that `LatencyProfile.h`
 * flags as "needs ADPF, not more queue".
 *
 * The NDK `APerformanceHint_*` symbols first shipped in API 33. minSdk here is
 * 29, so we resolve them with dlsym at runtime and degrade to a no-op when they
 * are unavailable (older devices, or non-Android hosts where the host test
 * suite compiles this header). No behavior change on those platforms — the
 * existing SCHED_FIFO / nice fallback stays exactly as-is.
 *
 * RT-safety: reportActualWorkDuration() forwards a single function-pointer call
 * once per DSP iteration. This mirrors the documented ADPF usage for game
 * render threads (reported every frame from the render thread); the framework
 * rate-limits internally. No allocation, no lock on the hot path here.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace watermelon_audio {

class AdpfSession {
public:
    AdpfSession() = default;
    ~AdpfSession() { close(); }

    AdpfSession(const AdpfSession&) = delete;
    AdpfSession& operator=(const AdpfSession&) = delete;

    /**
     * Create a hint session covering @p threadIds (Linux tids) with an expected
     * per-cycle work budget of @p targetNanos. Returns true only if a real ADPF
     * session was created; false means the object is an inert no-op (all methods
     * safe to call).
     */
    bool init(const std::vector<int32_t>& threadIds, int64_t targetNanos) {
        close();
        if (threadIds.empty() || targetNanos <= 0) {
            return false;
        }
        const Api& api = api_();
        if (!api.available()) {
            return false;
        }
        void* manager = api.getManager();
        if (manager == nullptr) {
            return false;
        }
        mSession = api.createSession(manager, threadIds.data(), threadIds.size(),
                                     targetNanos);
        mTargetNanos = targetNanos;
        return mSession != nullptr;
    }

    bool isActive() const { return mSession != nullptr; }

    /** True when the ADPF NDK symbols resolved (API 33+). Independent of any
     *  session — lets callers distinguish "unavailable" from "available but not
     *  active" for diagnostics. */
    static bool isSupported() { return api_().available(); }

    /** Report the wall-clock duration of the last work cycle. No-op if inert. */
    void reportActualWorkDuration(int64_t actualNanos) {
        if (mSession == nullptr || actualNanos <= 0) {
            return;
        }
        api_().reportActual(mSession, actualNanos);
    }

    /** Update the per-cycle target (e.g. after a latency-profile change). */
    void updateTargetWorkDuration(int64_t targetNanos) {
        if (mSession == nullptr || targetNanos <= 0 || targetNanos == mTargetNanos) {
            return;
        }
        api_().updateTarget(mSession, targetNanos);
        mTargetNanos = targetNanos;
    }

    void close() {
        if (mSession != nullptr) {
            api_().closeSession(mSession);
            mSession = nullptr;
        }
    }

private:
    // Opaque APerformanceHint types are treated as void here (only pointers).
    using GetManagerFn = void* (*)();
    using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
    using UpdateTargetFn = int (*)(void*, int64_t);
    using ReportActualFn = int (*)(void*, int64_t);
    using CloseSessionFn = void (*)(void*);

    struct Api {
        GetManagerFn getManager = nullptr;
        CreateSessionFn createSession = nullptr;
        UpdateTargetFn updateTarget = nullptr;
        ReportActualFn reportActual = nullptr;
        CloseSessionFn closeSession = nullptr;

        bool available() const {
            return getManager && createSession && updateTarget && reportActual &&
                   closeSession;
        }
    };

    // Resolved once, process-wide. Function-local static → thread-safe init.
    static const Api& api_() {
        static const Api api = load();
        return api;
    }

    static Api load() {
        Api api;
#if defined(__ANDROID__)
        void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
            return api;
        }
        api.getManager = reinterpret_cast<GetManagerFn>(
            dlsym(lib, "APerformanceHint_getManager"));
        api.createSession = reinterpret_cast<CreateSessionFn>(
            dlsym(lib, "APerformanceHint_createSession"));
        api.updateTarget = reinterpret_cast<UpdateTargetFn>(
            dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
        api.reportActual = reinterpret_cast<ReportActualFn>(
            dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
        api.closeSession = reinterpret_cast<CloseSessionFn>(
            dlsym(lib, "APerformanceHint_closeSession"));
        // The libandroid.so handle is intentionally not closed: it lives for the
        // whole process and the resolved pointers must stay valid.
#endif
        return api;
    }

    void* mSession = nullptr;
    int64_t mTargetNanos = 0;
};

} // namespace watermelon_audio
