#ifndef DELAYLINE_H
#define DELAYLINE_H

#include <vector>
#include <atomic>
#include <algorithm>
#include <cmath>
#include "DSPMath.h"

/**
 * @file DelayLine.h
 * @brief Circular buffer delay line with interpolation
 *
 * Provides fractional delay reading using linear or cubic interpolation.
 * Used for:
 * - Pre-delay
 * - Early reflections (multi-tap)
 * - Modulated delays (chorus/flanger/reverb)
 * - Comb filters
 *
 * Thread-safe: Write position is atomic, can be read from RT thread
 */
class DelayLine {
public:
    /**
     * @brief Interpolation method for fractional delay reads
     */
    enum class Interpolation {
        NONE,    ///< No interpolation (round to nearest sample)
        LINEAR,  ///< Linear interpolation (good quality, low CPU)
        CUBIC    ///< Cubic interpolation (best quality, higher CPU)
    };

    /**
     * @brief Constructor
     * @param maxDelayMs Maximum delay time in milliseconds
     * @param sampleRate Sample rate in Hz
     */
    DelayLine(float maxDelayMs = 100.0f, float sampleRate = 48000.0f);

    // Move semantics (for use in containers)
    DelayLine(DelayLine&& other) noexcept;
    DelayLine& operator=(DelayLine&& other) noexcept;

    // Delete copy constructor/assignment (prevent accidental copies)
    DelayLine(const DelayLine&) = delete;
    DelayLine& operator=(const DelayLine&) = delete;

    /**
     * @brief Set sample rate and resize buffer if needed
     * @param sampleRate Sample rate in Hz
     *
     * Warning: Clears the buffer! Call during initialization or when stopped.
     */
    void setSampleRate(float sampleRate);

    /**
     * @brief Set maximum delay capacity
     * @param maxDelayMs Maximum delay in milliseconds
     *
     * Warning: Clears the buffer! Call during initialization or when stopped.
     */
    void setMaxDelay(float maxDelayMs);

    /**
     * @brief Write a sample to the delay line
     * @param input Sample to write
     *
     * RT-safe: O(1) operation
     */
    void write(float input);

    /**
     * @brief Read a delayed sample with integer delay
     * @param delaySamples Delay in samples (integer)
     * @return Delayed sample
     *
     * RT-safe: O(1) operation
     * If delaySamples > buffer size, clamps to max delay
     */
    float read(int delaySamples) const;

    /**
     * @brief Read a delayed sample with fractional delay
     * @param delaySamples Delay in samples (can be fractional)
     * @param interpolation Interpolation method
     * @return Delayed sample
     *
     * RT-safe: O(1) for LINEAR, O(1) for CUBIC
     * Fractional delays are smoothly interpolated
     */
    float readInterpolated(float delaySamples,
                          Interpolation interpolation = Interpolation::LINEAR) const;

    /**
     * @brief Read a delayed sample in milliseconds
     * @param delayMs Delay in milliseconds
     * @param interpolation Interpolation method
     * @return Delayed sample
     *
     * Convenience method that converts ms to samples
     */
    float readMs(float delayMs,
                Interpolation interpolation = Interpolation::LINEAR) const;

    /**
     * @brief Tap the delay line at multiple positions (for early reflections)
     * @param delaySamples Array of delay times in samples
     * @param gains Array of gains for each tap
     * @param numTaps Number of taps
     * @return Mixed output of all taps
     *
     * RT-safe: Optimized for multiple reads
     */
    float readMultiTap(const int* delaySamples, const float* gains, int numTaps) const;

    /**
     * @brief Process input through delay and return delayed output
     * @param input Input sample
     * @param delaySamples Delay in samples (can be fractional)
     * @param interpolation Interpolation method
     * @return Delayed output
     *
     * Convenience method: write + readInterpolated in one call
     */
    float process(float input, float delaySamples,
                 Interpolation interpolation = Interpolation::LINEAR);

    /**
     * @brief Clear the delay buffer (reset to silence)
     *
     * RT-safe: Can be called during processing
     */
    void clear();

    /**
     * @brief Get maximum delay capacity in samples
     */
    int getMaxDelaySamples() const { return static_cast<int>(mBuffer.size()); }

    /**
     * @brief Get maximum delay capacity in milliseconds
     */
    float getMaxDelayMs() const;

    /**
     * @brief Get current write position
     */
    int getWritePosition() const {
        return mWritePos.load(std::memory_order_acquire);
    }

private:
    std::vector<float> mBuffer;          ///< Circular buffer
    std::atomic<int> mWritePos{0};       ///< Current write position (atomic)
    float mSampleRate{48000.0f};         ///< Sample rate in Hz

    /**
     * @brief Calculate read position from write position and delay
     * @param delaySamples Delay in samples
     * @return Read position in buffer (wrapped)
     */
    int calculateReadPos(int delaySamples) const;

    /**
     * @brief Get sample from buffer at given position
     * @param position Buffer index
     * @return Sample value
     *
     * Thread-safe read with bounds checking
     */
    float getSample(int position) const;
};

#endif // DELAYLINE_H
