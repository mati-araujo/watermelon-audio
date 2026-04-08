#ifndef DC_BLOCKER_H
#define DC_BLOCKER_H

/**
 * @file DCBlocker.h
 * @brief Simple DC offset removal filter
 *
 * Implements a high-pass filter at very low frequency (~0.1 Hz) to remove
 * DC offset from audio signals without affecting audible frequencies.
 */

/**
 * @class DCBlocker
 * @brief DC offset removal filter using a simple first-order highpass
 *
 * Uses the difference equation:
 *   y[n] = x[n] - x[n-1] + R * y[n-1]
 * where R is a coefficient very close to 1.0 (typically 0.995-0.999)
 *
 * This creates a highpass filter with cutoff frequency:
 *   fc = (1 - R) * sampleRate / (2 * π)
 *
 * Example: R = 0.995 at 48kHz gives fc ≈ 3.8 Hz
 */
class DCBlocker {
public:
    /**
     * @brief Constructor with optional coefficient
     * @param coefficient Pole coefficient [0.9, 1.0). Default: 0.995
     *                   Higher values = lower cutoff frequency
     */
    explicit DCBlocker(float coefficient = 0.995f)
        : mCoefficient(coefficient)
        , mX1(0.0f)
        , mY1(0.0f) {
        // Clamp coefficient to safe range
        if (mCoefficient < 0.9f) mCoefficient = 0.9f;
        if (mCoefficient >= 1.0f) mCoefficient = 0.999f;
    }

    /**
     * @brief Process a single sample
     * @param input Input sample
     * @return Filtered sample with DC offset removed
     */
    inline float process(float input) {
        float output = input - mX1 + mCoefficient * mY1;
        mX1 = input;
        mY1 = output;
        return output;
    }

    /**
     * @brief Process a buffer of samples (mono)
     * @param buffer Input/output buffer
     * @param numSamples Number of samples to process
     */
    inline void process(float* buffer, int numSamples) {
        for (int i = 0; i < numSamples; ++i) {
            buffer[i] = process(buffer[i]);
        }
    }

    /**
     * @brief Process a stereo buffer (interleaved L/R)
     * @param buffer Input/output buffer (interleaved stereo)
     * @param numFrames Number of frames (each frame = 2 samples)
     */
    inline void processStereo(float* buffer, int numFrames) {
        for (int i = 0; i < numFrames * 2; i += 2) {
            // Process left and right channels independently
            buffer[i] = process(buffer[i]);
            buffer[i + 1] = process(buffer[i + 1]);
        }
    }

    /**
     * @brief Reset the filter state
     */
    inline void reset() {
        mX1 = 0.0f;
        mY1 = 0.0f;
    }

    /**
     * @brief Set the cutoff frequency
     * @param cutoffHz Desired cutoff frequency in Hz (0.1 - 20 Hz recommended)
     * @param sampleRate Sample rate in Hz
     */
    inline void setCutoffFrequency(float cutoffHz, float sampleRate) {
        // Calculate coefficient from desired cutoff frequency
        // fc = (1 - R) * fs / (2 * π)
        // R = 1 - (2 * π * fc / fs)
        const float pi = 3.14159265359f;
        mCoefficient = 1.0f - (2.0f * pi * cutoffHz / sampleRate);

        // Clamp to safe range
        if (mCoefficient < 0.9f) mCoefficient = 0.9f;
        if (mCoefficient >= 1.0f) mCoefficient = 0.999f;
    }

private:
    float mCoefficient;  ///< Pole coefficient
    float mX1;           ///< Previous input sample
    float mY1;           ///< Previous output sample
};

/**
 * @class StereoDCBlocker
 * @brief Stereo DC blocker with independent left/right channels
 *
 * More efficient than processing stereo interleaved, as it maintains
 * separate state for each channel.
 */
class StereoDCBlocker {
public:
    /**
     * @brief Constructor with optional coefficient
     * @param coefficient Pole coefficient [0.9, 1.0). Default: 0.995
     */
    explicit StereoDCBlocker(float coefficient = 0.995f)
        : mLeftBlocker(coefficient)
        , mRightBlocker(coefficient) {
    }

    /**
     * @brief Prepare the DC blocker (sets cutoff frequency based on sample rate)
     * @param sampleRate Sample rate in Hz
     */
    void prepare(int sampleRate) {
        // Set cutoff to ~5 Hz for effective DC removal
        setCutoffFrequency(5.0f, static_cast<float>(sampleRate));
    }

    /**
     * @brief Process stereo interleaved buffer
     * @param buffer Input/output buffer (interleaved L/R)
     * @param numFrames Number of frames
     */
    inline void process(float* buffer, int numFrames) {
        for (int i = 0; i < numFrames * 2; i += 2) {
            buffer[i] = mLeftBlocker.process(buffer[i]);       // Left
            buffer[i + 1] = mRightBlocker.process(buffer[i + 1]); // Right
        }
    }

    /**
     * @brief Reset both channels
     */
    inline void reset() {
        mLeftBlocker.reset();
        mRightBlocker.reset();
    }

    /**
     * @brief Set cutoff frequency for both channels
     * @param cutoffHz Desired cutoff frequency in Hz
     * @param sampleRate Sample rate in Hz
     */
    inline void setCutoffFrequency(float cutoffHz, float sampleRate) {
        mLeftBlocker.setCutoffFrequency(cutoffHz, sampleRate);
        mRightBlocker.setCutoffFrequency(cutoffHz, sampleRate);
    }

private:
    DCBlocker mLeftBlocker;   ///< DC blocker for left channel
    DCBlocker mRightBlocker;  ///< DC blocker for right channel
};

#endif // DC_BLOCKER_H
