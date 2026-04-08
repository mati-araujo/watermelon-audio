#pragma once

#include <atomic>
#include <array>
#include <cstddef>

/**
 * @class LockFreeEventQueue
 * @brief Lock-free SPSC (single-producer, single-consumer) queue for event objects
 *
 * Fixed-capacity ring buffer using atomic indices. No heap allocation,
 * no mutexes — safe for real-time audio threads.
 *
 * Thread safety:
 * - One thread pushes (UI thread)
 * - One thread pops (audio thread)
 * - No locks needed
 *
 * @tparam T Event type (must be trivially copyable)
 * @tparam Capacity Maximum number of events in flight
 */
template<typename T, size_t Capacity>
class LockFreeEventQueue {
    static_assert(Capacity > 0, "Capacity must be > 0");
    // Use Capacity+1 internally to distinguish full from empty
    static constexpr size_t kBufferSize = Capacity + 1;

public:
    LockFreeEventQueue() : mWriteIndex(0), mReadIndex(0) {}

    /**
     * @brief Push an event (called from producer/UI thread)
     * @param event Event to enqueue
     * @return true if pushed, false if queue is full (event dropped)
     */
    bool push(const T& event) {
        size_t writeIdx = mWriteIndex.load(std::memory_order_relaxed);
        size_t nextWrite = (writeIdx + 1) % kBufferSize;

        // Check if full
        if (nextWrite == mReadIndex.load(std::memory_order_acquire)) {
            return false;  // Queue full — drop event
        }

        mBuffer[writeIdx] = event;

        // Release: ensure the event write is visible before advancing the index
        mWriteIndex.store(nextWrite, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pop an event (called from consumer/audio thread)
     * @param[out] event Event to dequeue into
     * @return true if an event was dequeued, false if queue is empty
     */
    bool pop(T& event) {
        size_t readIdx = mReadIndex.load(std::memory_order_relaxed);

        // Check if empty
        if (readIdx == mWriteIndex.load(std::memory_order_acquire)) {
            return false;  // Queue empty
        }

        event = mBuffer[readIdx];

        // Release: ensure the event read completes before advancing the index
        mReadIndex.store((readIdx + 1) % kBufferSize, std::memory_order_release);
        return true;
    }

    /**
     * @brief Check if there are events available (can be called from any thread)
     * @return true if non-empty
     */
    bool hasEvents() const {
        return mReadIndex.load(std::memory_order_acquire)
            != mWriteIndex.load(std::memory_order_acquire);
    }

    /**
     * @brief Get approximate number of pending events
     * @return Number of events (approximate under concurrent access)
     */
    size_t size() const {
        size_t w = mWriteIndex.load(std::memory_order_acquire);
        size_t r = mReadIndex.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (kBufferSize - r + w);
    }

    /**
     * @brief Clear all events (call only when both threads are idle, or from producer)
     */
    void clear() {
        mReadIndex.store(mWriteIndex.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
    }

private:
    std::array<T, kBufferSize> mBuffer;
    std::atomic<size_t> mWriteIndex;
    std::atomic<size_t> mReadIndex;
};
