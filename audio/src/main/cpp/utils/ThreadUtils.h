#pragma once

/**
 * @file ThreadUtils.h
 * @brief Thread utilities for real-time audio processing
 *
 * Provides functions for:
 * - Setting thread priority to real-time (SCHED_FIFO/SCHED_RR)
 * - Setting CPU affinity to pin threads to specific cores
 * - Naming threads for debugging
 *
 * Usage:
 *   std::thread audioThread(...);
 *   ThreadUtils::setRealtimeAudioThread(audioThread, "AudioDSP");
 */

#include <thread>
#include <string>

#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#endif

#include "../platform/Logger.h"

#define THREAD_LOG_TAG "ThreadUtils"
#define THREAD_LOGI(...) wma::logMessage(wma::LogLevel::INFO, THREAD_LOG_TAG, __VA_ARGS__)
#define THREAD_LOGW(...) wma::logMessage(wma::LogLevel::WARN, THREAD_LOG_TAG, __VA_ARGS__)
#define THREAD_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, THREAD_LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {

class ThreadUtils {
public:
    /**
     * Priority levels for audio threads
     */
    enum class Priority {
        NORMAL,         // Default priority
        ELEVATED,       // Above normal, for UI-related audio
        HIGH,           // High priority for audio processing
        REALTIME        // Maximum priority for critical audio paths
    };

    /**
     * Actual scheduling outcome of a realtime-priority request. Android usually
     * denies SCHED_FIFO/SCHED_RR to non-privileged apps and we fall back to a
     * nice value — callers that need real-time guarantees (Fase 3 microframe)
     * must be able to observe which happened instead of assuming success.
     */
    enum class SchedResult {
        FIFO_GRANTED,   // SCHED_FIFO applied — hard real-time
        RR_GRANTED,     // SCHED_RR applied
        NICE_FALLBACK,  // RT policy denied; nice value applied (typical Android)
        FAILED          // Could not set any scheduling parameters
    };

    static const char* toString(SchedResult r) {
        switch (r) {
            case SchedResult::FIFO_GRANTED:  return "FIFO_GRANTED";
            case SchedResult::RR_GRANTED:    return "RR_GRANTED";
            case SchedResult::NICE_FALLBACK: return "NICE_FALLBACK";
            case SchedResult::FAILED:        return "FAILED";
        }
        return "UNKNOWN";
    }

    /**
     * @brief Configure a thread for real-time audio processing
     *
     * Sets the thread to:
     * - SCHED_FIFO or SCHED_RR scheduling policy
     * - Maximum or high priority
     * - Named for debugging (visible in systrace/profilers)
     *
     * @param thread The std::thread to configure (must be joinable)
     * @param name Thread name (max 15 chars on Linux)
     * @param priority Priority level to set
     * @return true if all settings were applied successfully
     */
    static bool setRealtimeAudioThread(std::thread& thread, const char* name,
                                        Priority priority = Priority::REALTIME) {
        if (!thread.joinable()) {
            THREAD_LOGE("Cannot configure non-joinable thread");
            return false;
        }

        pthread_t handle = thread.native_handle();
        bool success = true;

        // 1. Set thread name
        if (!setThreadName(handle, name)) {
            THREAD_LOGW("Failed to set thread name: %s", name);
            success = false;
        }

        // 2. Set scheduling priority
        if (setThreadPriority(handle, priority) == SchedResult::FAILED) {
            THREAD_LOGW("Failed to set thread priority for: %s", name);
            success = false;
        }

        if (success) {
            THREAD_LOGI("Configured realtime thread: %s", name);
        }

        return success;
    }

    /**
     * @brief Configure the current thread for real-time audio
     *
     * Same as setRealtimeAudioThread but for the calling thread.
     *
     * @return the actual scheduling outcome (SCHED_FIFO granted vs. nice
     *         fallback). Callers that need hard real-time guarantees inspect
     *         this instead of assuming the request succeeded.
     */
    static SchedResult setCurrentThreadRealtime(const char* name,
                                                Priority priority = Priority::REALTIME) {
        pthread_t handle = pthread_self();
        setThreadName(handle, name);
        return setThreadPriority(handle, priority);
    }

    /**
     * @brief Pin a thread to a specific CPU core
     *
     * Helps reduce cache misses and context switch overhead
     * by keeping the thread on the same core.
     *
     * @param thread The thread to pin
     * @param coreId The CPU core ID (0-based)
     * @return true if affinity was set successfully
     */
    static bool setCpuAffinity(std::thread& thread, int coreId) {
        if (!thread.joinable()) {
            return false;
        }
        return setCpuAffinityInternal(thread.native_handle(), coreId);
    }

    /**
     * @brief Pin the current thread to a specific CPU core
     */
    static bool setCurrentThreadCpuAffinity(int coreId) {
        return setCpuAffinityInternal(pthread_self(), coreId);
    }

    /**
     * @brief Pin a thread to a set of CPU cores (big cores for performance)
     *
     * On ARM big.LITTLE, this pins to the big/performance cores.
     * Returns the first core ID used.
     */
    static int pinToPerformanceCores(std::thread& thread) {
        if (!thread.joinable()) {
            return -1;
        }

        // On most ARM SoCs, big cores are the higher-numbered ones
        // Get number of CPUs
        int numCpus = getNumCpus();
        if (numCpus <= 0) {
            return -1;
        }

        // Use the last core (typically a big core on ARM big.LITTLE)
        // For 8-core: cores 4-7 are usually big cores
        int targetCore = numCpus - 1;

        // Try to use a big core (upper half of cores)
        if (numCpus >= 4) {
            targetCore = numCpus / 2 + (numCpus / 4); // e.g., core 6 on 8-core
        }

        if (setCpuAffinityInternal(thread.native_handle(), targetCore)) {
            THREAD_LOGI("Pinned thread to core %d (of %d)", targetCore, numCpus);
            return targetCore;
        }

        return -1;
    }

    /**
     * @brief Get number of available CPU cores
     */
    static int getNumCpus() {
#if defined(__ANDROID__) || defined(__linux__)
        return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
#else
        return std::thread::hardware_concurrency();
#endif
    }

private:
    static bool setThreadName(pthread_t handle, const char* name) {
#if defined(__ANDROID__) || defined(__linux__)
        // Linux/Android limit is 16 chars including null terminator
        char truncatedName[16];
        strncpy(truncatedName, name, 15);
        truncatedName[15] = '\0';

        int result = pthread_setname_np(handle, truncatedName);
        return result == 0;
#else
        (void)handle;
        (void)name;
        return true;
#endif
    }

    static SchedResult setThreadPriority(pthread_t handle, Priority priority) {
#if defined(__ANDROID__) || defined(__linux__)
        // First, try to use Android's nice value (works without root)
        // Lower nice = higher priority, range is -20 to 19
        int niceValue = 0;
        switch (priority) {
            case Priority::NORMAL:
                niceValue = 0;
                break;
            case Priority::ELEVATED:
                niceValue = -5;
                break;
            case Priority::HIGH:
                niceValue = -10;
                break;
            case Priority::REALTIME:
                niceValue = -19; // Highest non-root priority
                break;
        }

        // Set nice value for this thread
        // Note: setpriority with PRIO_PROCESS and tid=0 sets current thread
        pid_t tid = syscall(SYS_gettid);
        bool niceApplied = (setpriority(PRIO_PROCESS, tid, niceValue) == 0);
        if (niceApplied) {
            THREAD_LOGI("Set nice value to %d for tid %d", niceValue, tid);
        }

        // Try to set real-time scheduling (may fail without CAP_SYS_NICE)
        struct sched_param param;
        int policy;
        int maxPriority;

        switch (priority) {
            case Priority::REALTIME:
                policy = SCHED_FIFO;
                maxPriority = sched_get_priority_max(SCHED_FIFO);
                param.sched_priority = maxPriority;
                break;

            case Priority::HIGH:
                policy = SCHED_RR;
                maxPriority = sched_get_priority_max(SCHED_RR);
                param.sched_priority = maxPriority * 3 / 4;
                break;

            case Priority::ELEVATED:
                policy = SCHED_RR;
                maxPriority = sched_get_priority_max(SCHED_RR);
                param.sched_priority = maxPriority / 2;
                break;

            default:
                // NORMAL priority - don't change scheduler. The nice value (0)
                // is the only lever; report it as the "fallback" outcome.
                return niceApplied ? SchedResult::NICE_FALLBACK : SchedResult::FAILED;
        }

        int result = pthread_setschedparam(handle, policy, &param);
        if (result != 0) {
            // Expected to fail on Android without root. The nice value we set
            // earlier still applies, so this is a graceful fallback, not an
            // error — but callers can distinguish it from a granted RT policy.
            THREAD_LOGW("pthread_setschedparam failed (errno=%d), using nice value only", result);
            return niceApplied ? SchedResult::NICE_FALLBACK : SchedResult::FAILED;
        }

        THREAD_LOGI("Set scheduling policy=%d, priority=%d", policy, param.sched_priority);
        return (policy == SCHED_FIFO) ? SchedResult::FIFO_GRANTED
                                      : SchedResult::RR_GRANTED;
#else
        (void)handle;
        (void)priority;
        return SchedResult::NICE_FALLBACK;
#endif
    }

    static bool setCpuAffinityInternal(pthread_t handle, int coreId) {
#if defined(__ANDROID__)
        // Android uses sched_setaffinity instead of pthread_setaffinity_np
        // We need to get the thread's Linux TID
        int numCpus = getNumCpus();
        if (coreId < 0 || coreId >= numCpus) {
            THREAD_LOGE("Invalid core ID: %d (max: %d)", coreId, numCpus - 1);
            return false;
        }

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);

        // On Android, use sched_setaffinity with tid=0 for current thread
        // For other threads, we need to get the TID which isn't straightforward
        // from pthread_t. We'll set affinity from within the thread instead.
        pid_t tid = syscall(SYS_gettid);
        int result = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
        if (result != 0) {
            THREAD_LOGW("Failed to set CPU affinity to core %d (errno=%d)", coreId, errno);
            return false;
        }

        return true;
#elif defined(__linux__)
        int numCpus = getNumCpus();
        if (coreId < 0 || coreId >= numCpus) {
            THREAD_LOGE("Invalid core ID: %d (max: %d)", coreId, numCpus - 1);
            return false;
        }

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);

        int result = pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset);
        if (result != 0) {
            THREAD_LOGW("Failed to set CPU affinity to core %d (errno=%d)", coreId, result);
            return false;
        }

        return true;
#else
        (void)handle;
        (void)coreId;
        return true;
#endif
    }
};

} // namespace watermelon_audio
