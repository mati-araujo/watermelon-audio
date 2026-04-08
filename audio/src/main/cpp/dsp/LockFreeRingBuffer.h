#pragma once

#include <atomic>
#include <vector>
#include <cstring>

/**
 * @class LockFreeRingBuffer
 * @brief Lock-free ring buffer for audio streaming between threads
 *
 * Designed for single-producer, single-consumer scenarios (SPSC).
 * Used to transfer audio data from input stream callback to output processing.
 *
 * Thread safety:
 * - One thread can write (input audio thread)
 * - One thread can read (output audio thread)
 * - No locks needed, uses atomic operations with appropriate memory ordering
 */
class LockFreeRingBuffer {
public:
    /**
     * @brief Constructor
     * @param capacity Buffer capacity in samples (not frames)
     */
    explicit LockFreeRingBuffer(size_t capacity)
        : mCapacity(capacity)
        , mBuffer(capacity, 0.0f)
        , mWriteIndex(0)
        , mReadIndex(0) {}

    /**
     * @brief Resize the buffer (only when not in use)
     * @param newCapacity New capacity in samples
     */
    void resize(size_t newCapacity) {
        mCapacity = newCapacity;
        mBuffer.resize(newCapacity, 0.0f);
        mWriteIndex.store(0, std::memory_order_relaxed);
        mReadIndex.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Write samples to the buffer (called from input thread)
     * @param data Pointer to sample data
     * @param numSamples Number of samples to write
     * @return true if write was successful, false on overflow
     */
    bool write(const float* data, size_t numSamples) {
        size_t writeIdx = mWriteIndex.load(std::memory_order_relaxed);
        size_t readIdx = mReadIndex.load(std::memory_order_acquire);

        size_t available = availableToWrite(writeIdx, readIdx);
        if (available < numSamples) {
            return false;  // Buffer overflow
        }

        // Write data with possible wrap-around
        size_t firstPart = std::min(numSamples, mCapacity - writeIdx);
        std::memcpy(mBuffer.data() + writeIdx, data, firstPart * sizeof(float));

        if (firstPart < numSamples) {
            // Wrap around to beginning
            std::memcpy(mBuffer.data(), data + firstPart,
                        (numSamples - firstPart) * sizeof(float));
        }

        // Update write index with release semantics
        size_t newWriteIdx = (writeIdx + numSamples) % mCapacity;
        mWriteIndex.store(newWriteIdx, std::memory_order_release);

        return true;
    }

    /**
     * @brief Read samples from the buffer (called from output thread)
     * @param data Pointer to output buffer
     * @param numSamples Number of samples to read
     * @return true if read was successful, false on underrun (buffer filled with silence)
     */
    bool read(float* data, size_t numSamples) {
        size_t writeIdx = mWriteIndex.load(std::memory_order_acquire);
        size_t readIdx = mReadIndex.load(std::memory_order_relaxed);

        size_t available = availableToRead(writeIdx, readIdx);
        if (available < numSamples) {
            // Underrun: fill with silence
            std::memset(data, 0, numSamples * sizeof(float));
            return false;
        }

        // Read data with possible wrap-around
        size_t firstPart = std::min(numSamples, mCapacity - readIdx);
        std::memcpy(data, mBuffer.data() + readIdx, firstPart * sizeof(float));

        if (firstPart < numSamples) {
            // Wrap around
            std::memcpy(data + firstPart, mBuffer.data(),
                        (numSamples - firstPart) * sizeof(float));
        }

        // Update read index with release semantics
        size_t newReadIdx = (readIdx + numSamples) % mCapacity;
        mReadIndex.store(newReadIdx, std::memory_order_release);

        return true;
    }

    /**
     * @brief Get number of samples available to read
     * @return Available samples for reading
     */
    size_t availableToRead() const {
        return availableToRead(
            mWriteIndex.load(std::memory_order_acquire),
            mReadIndex.load(std::memory_order_relaxed)
        );
    }

    /**
     * @brief Get number of samples that can be written
     * @return Available space for writing
     */
    size_t availableToWrite() const {
        return availableToWrite(
            mWriteIndex.load(std::memory_order_relaxed),
            mReadIndex.load(std::memory_order_acquire)
        );
    }

    /**
     * @brief Clear the buffer (reset read/write indices)
     */
    void clear() {
        mWriteIndex.store(0, std::memory_order_relaxed);
        mReadIndex.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Get buffer capacity
     * @return Capacity in samples
     */
    size_t capacity() const { return mCapacity; }

    /**
     * @brief Get pointer to internal buffer data
     * @return Pointer to buffer data (for memory locking)
     */
    float* data() { return mBuffer.data(); }
    const float* data() const { return mBuffer.data(); }

    /**
     * @brief Get buffer size in bytes
     * @return Size in bytes (for memory locking)
     */
    size_t sizeBytes() const { return mBuffer.size() * sizeof(float); }

private:
    size_t availableToRead(size_t writeIdx, size_t readIdx) const {
        if (writeIdx >= readIdx) {
            return writeIdx - readIdx;
        }
        return mCapacity - readIdx + writeIdx;
    }

    size_t availableToWrite(size_t writeIdx, size_t readIdx) const {
        // Leave one space to distinguish full from empty
        return mCapacity - 1 - availableToRead(writeIdx, readIdx);
    }

private:
    size_t mCapacity;
    std::vector<float> mBuffer;
    std::atomic<size_t> mWriteIndex;
    std::atomic<size_t> mReadIndex;
};
