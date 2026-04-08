#ifndef OVERSAMPLER_H
#define OVERSAMPLER_H

#include <vector>
#include <atomic>
#include <cmath>
#include <algorithm>
#include "BiquadFilter.h"
#include "DSPMath.h"

/**
 * @file Oversampler.h
 * @brief Anti-aliasing oversampler for distortion effects
 *
 * Provides 2x and 4x oversampling to prevent aliasing artifacts
 * when applying non-linear waveshaping (distortion, saturation).
 *
 * Flow:
 *   Input → Upsample → [Process at higher rate] → Downsample → Output
 *
 * The upsampling inserts zeros between samples and applies a lowpass
 * filter. The downsampling applies a lowpass anti-aliasing filter
 * and then decimates.
 *
 * Thread-safe: Oversampling factor can be changed from UI thread.
 */
class Oversampler {
public:
    /**
     * @brief Oversampling factor
     */
    enum class Factor {
        X1 = 1,     ///< No oversampling (bypass)
        X2 = 2,     ///< 2x oversampling
        X4 = 4      ///< 4x oversampling
    };

    /**
     * @brief Constructor
     * @param maxBlockSize Maximum input block size
     */
    explicit Oversampler(int maxBlockSize = 1024);

    /**
     * @brief Prepare for processing
     * @param sampleRate Base sample rate in Hz
     * @param blockSize Expected block size
     */
    void prepare(float sampleRate, int blockSize);

    /**
     * @brief Set oversampling factor
     * @param factor X1, X2, or X4
     *
     * Thread-safe: Can be called from UI thread
     */
    void setFactor(Factor factor);

    /**
     * @brief Get current oversampling factor
     */
    Factor getFactor() const {
        return static_cast<Factor>(mFactor.load(std::memory_order_relaxed));
    }

    /**
     * @brief Get the upsampled block size
     * @param inputSize Original block size
     * @return Size of upsampled buffer
     */
    int getUpsampledSize(int inputSize) const {
        return inputSize * mFactor.load(std::memory_order_relaxed);
    }

    /**
     * @brief Upsample input buffer
     * @param input Input buffer (original rate)
     * @param output Output buffer (must be factor * numSamples size)
     * @param numSamples Number of input samples
     *
     * After upsampling, process the output buffer at the higher rate,
     * then call downsample() to return to the original rate.
     */
    void upsample(const float* input, float* output, int numSamples);

    /**
     * @brief Downsample with anti-aliasing
     * @param input Input buffer (upsampled rate)
     * @param output Output buffer (original rate)
     * @param numSamples Number of OUTPUT samples
     *
     * Input size must be numSamples * factor
     */
    void downsample(const float* input, float* output, int numSamples);

    /**
     * @brief Get pointer to internal upsampled buffer
     * @return Pointer to working buffer (for in-place processing)
     *
     * Buffer size is maxBlockSize * 4 (for X4 oversampling)
     */
    float* getBuffer() {
        return mWorkBuffer.data();
    }

    /**
     * @brief Get upsampled sample rate
     * @return Sample rate * factor
     */
    float getUpsampledRate() const {
        return mSampleRate * static_cast<float>(mFactor.load(std::memory_order_relaxed));
    }

    /**
     * @brief Reset filter states
     */
    void reset();

private:
    float mSampleRate{48000.0f};
    int mMaxBlockSize;

    // Current factor (atomic for thread-safety)
    std::atomic<int> mFactor{1};

    // Working buffers (pre-allocated)
    std::vector<float> mWorkBuffer;      // For upsampled audio
    std::vector<float> mTempBuffer;      // For intermediate stages

    // Anti-aliasing filters (cascaded for steep rolloff)
    // For 2x: cutoff at original_rate/2
    // For 4x: cutoff at original_rate/2
    BiquadFilter mUpsampleFilter1;
    BiquadFilter mUpsampleFilter2;
    BiquadFilter mDownsampleFilter1;
    BiquadFilter mDownsampleFilter2;

    /**
     * @brief Update filter cutoffs for current sample rate and factor
     */
    void updateFilters();
};

#endif // OVERSAMPLER_H
