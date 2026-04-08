#pragma once

/**
 * @file WaveformCapture.h
 * @brief RT-safe double-buffered waveform capture for visualization.
 *
 * Audio thread writes to the INACTIVE buffer, then swaps atomically.
 * UI thread reads from the ACTIVE buffer — no contention, no locks.
 *
 * Phase 1E: Extracted from AudioEngine.
 */

#include <vector>
#include <atomic>
#include <cstring>
#include <algorithm>

class WaveformCapture {
public:
    explicit WaveformCapture(size_t capacity = 1024)
        : mBuffer1(capacity, 0.0f)
        , mBuffer2(capacity, 0.0f) {
    }

    /**
     * Resize both buffers. Call from constructor or non-RT thread only.
     */
    void resize(size_t capacity) {
        mBuffer1.resize(capacity, 0.0f);
        mBuffer2.resize(capacity, 0.0f);
        reset();
    }

    /**
     * Reset all write state. Safe from any thread (atomics).
     */
    void reset() {
        mActiveBuffer.store(0, std::memory_order_relaxed);
        mWriteIndex.store(0, std::memory_order_relaxed);
        mReadSize.store(0, std::memory_order_relaxed);
    }

    /**
     * Clear buffer contents + reset. Non-RT thread only.
     */
    void clear() {
        std::fill(mBuffer1.begin(), mBuffer1.end(), 0.0f);
        std::fill(mBuffer2.begin(), mBuffer2.end(), 0.0f);
        reset();
    }

    /**
     * Write interleaved stereo samples (left channel only) to inactive buffer.
     * When enough samples accumulate (>=256), swap buffers atomically.
     *
     * RT-safe: No allocations, no locks.
     * @param interleavedStereo Interleaved L/R audio data
     * @param numFrames Number of stereo frames
     */
    void write(const float* interleavedStereo, int numFrames) {
        int active = mActiveBuffer.load(std::memory_order_acquire);
        std::vector<float>& writeBuffer = (active == 0) ? mBuffer2 : mBuffer1;
        size_t capacity = writeBuffer.size();
        size_t writeIdx = mWriteIndex.load(std::memory_order_relaxed);

        // Write left channel samples
        for (int i = 0; i < numFrames; ++i) {
            if (writeIdx < capacity) {
                writeBuffer[writeIdx] = interleavedStereo[i * 2];
                ++writeIdx;
            }
        }

        // Swap when buffer has enough samples for smooth display
        if (writeIdx >= 256) {
            int newActive = (active == 0) ? 1 : 0;
            mReadSize.store(writeIdx, std::memory_order_release);
            mActiveBuffer.store(newActive, std::memory_order_release);
            mWriteIndex.store(0, std::memory_order_relaxed);
        } else {
            mWriteIndex.store(writeIdx, std::memory_order_relaxed);
        }
    }

    /**
     * Read the most recent waveform samples from the active buffer.
     * Lock-free: reads from buffer that audio thread is NOT writing to.
     *
     * @param buffer Output buffer (caller-owned)
     * @param maxSize Maximum samples to read
     * @return Number of samples actually written to buffer
     */
    int read(float* buffer, int maxSize) const {
        if (!buffer || maxSize <= 0) return 0;

        int active = mActiveBuffer.load(std::memory_order_acquire);
        const std::vector<float>& readBuffer = (active == 0) ? mBuffer1 : mBuffer2;

        size_t validSamples = mReadSize.load(std::memory_order_acquire);
        size_t capacity = readBuffer.size();

        size_t size = std::min({static_cast<size_t>(maxSize), capacity, validSamples});
        if (size == 0) return 0;

        // Copy most recent samples (from end of valid data)
        size_t startIdx = (validSamples >= size) ? (validSamples - size) : 0;
        for (size_t i = 0; i < size; ++i) {
            buffer[i] = readBuffer[startIdx + i];
        }

        return static_cast<int>(size);
    }

private:
    std::vector<float> mBuffer1;
    std::vector<float> mBuffer2;
    std::atomic<int> mActiveBuffer{0};      // 0=buffer1 active (UI reads), 1=buffer2 active
    std::atomic<size_t> mWriteIndex{0};     // Write position in inactive buffer
    std::atomic<size_t> mReadSize{0};       // Valid sample count in active buffer
};
