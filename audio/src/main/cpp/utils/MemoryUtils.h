#pragma once

/**
 * @file MemoryUtils.h
 * @brief Memory utilities for real-time audio processing
 *
 * Provides functions for:
 * - Locking memory pages to prevent swapping (mlock)
 * - Pre-faulting pages to avoid page faults during playback
 * - Allocating page-aligned memory
 *
 * Why this matters for audio:
 * - Page faults during audio callback cause glitches
 * - Swapped memory causes unpredictable latency
 * - Pre-touching ensures pages are resident before playback
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__ANDROID__) || defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../platform/Logger.h"

#define MEMORY_LOG_TAG "MemoryUtils"
#define MEMORY_LOGI(...) wma::logMessage(wma::LogLevel::INFO, MEMORY_LOG_TAG, __VA_ARGS__)
#define MEMORY_LOGW(...) wma::logMessage(wma::LogLevel::WARN, MEMORY_LOG_TAG, __VA_ARGS__)
#define MEMORY_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, MEMORY_LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {

class MemoryUtils {
public:
    /**
     * @brief Lock a memory region to prevent it from being swapped out
     *
     * Uses mlock() to keep the memory pages resident in RAM.
     * This prevents page faults during real-time audio processing.
     *
     * Note: On Android, mlock() may fail silently or only lock a portion
     * of memory depending on system limits (RLIMIT_MEMLOCK).
     *
     * @param address Pointer to the start of the memory region
     * @param size Size of the region in bytes
     * @return true if the lock was successful
     */
    static bool lockMemory(void* address, size_t size) {
        if (address == nullptr || size == 0) {
            return false;
        }

#if defined(__ANDROID__) || defined(__linux__)
        int result = mlock(address, size);
        if (result != 0) {
            // mlock often fails on Android due to RLIMIT_MEMLOCK
            // This is expected and not critical
            MEMORY_LOGW("mlock failed for %zu bytes (errno=%d) - this is expected on Android",
                        size, errno);
            return false;
        }

        MEMORY_LOGI("Locked %zu bytes of memory at %p", size, address);
        return true;
#else
        (void)address;
        (void)size;
        return true;
#endif
    }

    /**
     * @brief Unlock a previously locked memory region
     *
     * @param address Pointer to the start of the memory region
     * @param size Size of the region in bytes
     * @return true if the unlock was successful
     */
    static bool unlockMemory(void* address, size_t size) {
        if (address == nullptr || size == 0) {
            return false;
        }

#if defined(__ANDROID__) || defined(__linux__)
        int result = munlock(address, size);
        if (result != 0) {
            MEMORY_LOGW("munlock failed for %zu bytes", size);
            return false;
        }
        return true;
#else
        (void)address;
        (void)size;
        return true;
#endif
    }

    /**
     * @brief Pre-fault memory pages to ensure they are resident
     *
     * Touches each page in the memory region to trigger page faults now
     * rather than during audio playback. This is useful even if mlock fails.
     *
     * @param address Pointer to the start of the memory region
     * @param size Size of the region in bytes
     */
    static void prefaultPages(void* address, size_t size) {
        if (address == nullptr || size == 0) {
            return;
        }

#if defined(__ANDROID__) || defined(__linux__)
        size_t pageSize = getPageSize();
        volatile uint8_t* ptr = static_cast<volatile uint8_t*>(address);

        // Touch each page to ensure it's resident
        for (size_t offset = 0; offset < size; offset += pageSize) {
            // Read to trigger page fault if needed
            (void)ptr[offset];
        }

        // Also touch the last byte in case size isn't page-aligned
        if (size > 0) {
            (void)ptr[size - 1];
        }

        MEMORY_LOGI("Pre-faulted %zu bytes (%zu pages) at %p",
                    size, (size + pageSize - 1) / pageSize, address);
#else
        (void)address;
        (void)size;
#endif
    }

    /**
     * @brief Pre-fault and optionally lock memory pages
     *
     * Combines prefaultPages() and lockMemory() for convenience.
     * Even if locking fails, prefaulting still helps.
     *
     * @param address Pointer to the start of the memory region
     * @param size Size of the region in bytes
     * @return true if locking was successful (prefaulting always succeeds)
     */
    static bool prepareForRealtime(void* address, size_t size) {
        // Always prefault first
        prefaultPages(address, size);

        // Try to lock (may fail on Android, that's OK)
        return lockMemory(address, size);
    }

    /**
     * @brief Prepare a vector's data for real-time use
     *
     * @tparam T Element type
     * @param vec The vector to prepare
     * @return true if locking was successful
     */
    template<typename T>
    static bool prepareVectorForRealtime(std::vector<T>& vec) {
        if (vec.empty()) {
            return true;
        }
        return prepareForRealtime(vec.data(), vec.size() * sizeof(T));
    }

    /**
     * @brief Get the system page size
     */
    static size_t getPageSize() {
#if defined(__ANDROID__) || defined(__linux__)
        static size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        return pageSize;
#else
        return 4096; // Default assumption
#endif
    }

    /**
     * @brief Advise the kernel about memory usage patterns
     *
     * Tells the kernel to keep these pages in memory and expect
     * sequential access.
     *
     * @param address Pointer to the start of the memory region
     * @param size Size of the region in bytes
     */
    static void adviseWillNeed(void* address, size_t size) {
#if defined(__ANDROID__) || defined(__linux__)
        if (address == nullptr || size == 0) {
            return;
        }

        // Align to page boundary
        size_t pageSize = getPageSize();
        uintptr_t addr = reinterpret_cast<uintptr_t>(address);
        uintptr_t alignedAddr = addr & ~(pageSize - 1);
        size_t alignedSize = size + (addr - alignedAddr);

        // Tell kernel we'll need this memory soon
        int result = madvise(reinterpret_cast<void*>(alignedAddr), alignedSize, MADV_WILLNEED);
        if (result != 0) {
            MEMORY_LOGW("madvise(WILLNEED) failed");
        }

        // Tell kernel this is sequential access (helps prefetching)
        result = madvise(reinterpret_cast<void*>(alignedAddr), alignedSize, MADV_SEQUENTIAL);
        if (result != 0) {
            MEMORY_LOGW("madvise(SEQUENTIAL) failed");
        }
#else
        (void)address;
        (void)size;
#endif
    }

    /**
     * @brief Allocate page-aligned memory
     *
     * @param size Size to allocate
     * @return Pointer to aligned memory, or nullptr on failure
     */
    static void* allocateAligned(size_t size) {
#if defined(__ANDROID__) || defined(__linux__)
        void* ptr = nullptr;
        size_t alignment = getPageSize();

        int result = posix_memalign(&ptr, alignment, size);
        if (result != 0) {
            MEMORY_LOGE("posix_memalign failed for %zu bytes", size);
            return nullptr;
        }

        return ptr;
#else
        return malloc(size);
#endif
    }

    /**
     * @brief Free page-aligned memory
     */
    static void freeAligned(void* ptr) {
        free(ptr); // free() works for posix_memalign memory
    }
};

} // namespace watermelon_audio
