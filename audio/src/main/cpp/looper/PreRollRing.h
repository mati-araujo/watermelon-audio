#pragma once

#include <atomic>
#include <cstring>
#include <vector>

/**
 * @class PreRollRing
 * @brief Pre-allocated stereo circular buffer of recent post-FX audio.
 *
 * The AudioEngine writes the post-effects signal here on every audio callback,
 * just before the looper tap. When a recording is armed/started, the looper can
 * snapshot the last N milliseconds from this ring to seed the track with audio
 * that occurred BEFORE the user pressed REC — eliminating the human-reaction
 * gap at the start of a take.
 *
 * Threading:
 *  - prepare()/setSize() → UI thread (allocates).
 *  - write() → audio thread only (RT-safe, no allocations).
 *  - snapshot() → UI thread (lock-free read; may catch one in-flight write,
 *    bounded by `numFrames` of audio block — typically <10 ms — acceptable for
 *    pre-roll seeding which is inherently a "best effort" feature).
 */
class PreRollRing {
public:
    PreRollRing() = default;

    /**
     * @brief Allocate the ring. Call from UI thread.
     * @param maxFrames Capacity in stereo frames. Determines max pre-roll length.
     */
    void prepare(int maxFrames) {
        if (maxFrames <= 0) maxFrames = 0;
        mBuffer.assign(static_cast<size_t>(maxFrames) * 2, 0.0f);
        mCapacityFrames = maxFrames;
        mWritePos.store(0, std::memory_order_release);
        mTotalWritten.store(0, std::memory_order_release);
    }

    int getCapacityFrames() const { return mCapacityFrames; }

    /**
     * @brief Write `numFrames` of stereo audio into the ring. RT-safe.
     */
    void write(const float* stereoData, int numFrames) {
        if (mCapacityFrames <= 0 || !stereoData || numFrames <= 0) return;

        int writePos = mWritePos.load(std::memory_order_relaxed);
        // Two-segment write to handle wrap.
        int firstChunk = std::min(numFrames, mCapacityFrames - writePos);
        std::memcpy(mBuffer.data() + static_cast<size_t>(writePos) * 2,
                    stereoData,
                    static_cast<size_t>(firstChunk) * 2 * sizeof(float));

        if (firstChunk < numFrames) {
            int rem = numFrames - firstChunk;
            std::memcpy(mBuffer.data(),
                        stereoData + static_cast<size_t>(firstChunk) * 2,
                        static_cast<size_t>(rem) * 2 * sizeof(float));
            writePos = rem;
        } else {
            writePos += numFrames;
            if (writePos >= mCapacityFrames) writePos -= mCapacityFrames;
        }

        mWritePos.store(writePos, std::memory_order_release);
        mTotalWritten.fetch_add(static_cast<int64_t>(numFrames),
                                std::memory_order_release);
    }

    /**
     * @brief Snapshot the most recent `numFrames` frames into `outBuffer`.
     *        Pads the front with zeros if fewer frames have been written total.
     * @param outBuffer Stereo interleaved destination (caller-allocated, size = numFrames*2).
     * @param numFrames Number of frames to extract.
     * @return Number of frames written from the ring (numFrames, padded with leading zeros).
     */
    int snapshot(float* outBuffer, int numFrames) const {
        if (!outBuffer || numFrames <= 0 || mCapacityFrames <= 0) return 0;
        if (numFrames > mCapacityFrames) numFrames = mCapacityFrames;

        int64_t total = mTotalWritten.load(std::memory_order_acquire);
        int writePos = mWritePos.load(std::memory_order_acquire);

        // How many of the requested frames are actually available (i.e. ever written).
        int available = static_cast<int>(std::min<int64_t>(total, numFrames));
        int padFrames = numFrames - available;

        // Zero-pad the leading portion if we don't have enough history yet.
        if (padFrames > 0) {
            std::memset(outBuffer, 0,
                        static_cast<size_t>(padFrames) * 2 * sizeof(float));
        }

        // The most-recent `available` frames end at writePos in the ring.
        // Their start position is writePos - available (mod capacity).
        int readStart = writePos - available;
        if (readStart < 0) readStart += mCapacityFrames;

        int firstChunk = std::min(available, mCapacityFrames - readStart);
        std::memcpy(outBuffer + static_cast<size_t>(padFrames) * 2,
                    mBuffer.data() + static_cast<size_t>(readStart) * 2,
                    static_cast<size_t>(firstChunk) * 2 * sizeof(float));

        if (firstChunk < available) {
            int rem = available - firstChunk;
            std::memcpy(outBuffer + static_cast<size_t>(padFrames + firstChunk) * 2,
                        mBuffer.data(),
                        static_cast<size_t>(rem) * 2 * sizeof(float));
        }

        return numFrames;
    }

private:
    std::vector<float> mBuffer;
    int mCapacityFrames{0};
    std::atomic<int> mWritePos{0};
    std::atomic<int64_t> mTotalWritten{0};
};
