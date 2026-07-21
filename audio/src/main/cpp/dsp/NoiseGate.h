#pragma once

#include <algorithm>
#include <cmath>
#include <atomic>

/**
 * @class NoiseGate
 * @brief Noise gate with hysteresis for audio input processing
 *
 * Features:
 * - Configurable threshold with hysteresis to prevent chatter
 * - Smooth attack/release envelope
 * - Thread-safe parameter updates
 */
class NoiseGate {
public:
    NoiseGate() = default;

    /**
     * @brief Prepare the noise gate for processing
     * @param sampleRate Sample rate in Hz
     */
    void prepare(int sampleRate) {
        mSampleRate = sampleRate;
        updateCoefficients();
    }

    /**
     * @brief Set the gate threshold
     * @param thresholdDb Threshold in dB (typically -60 to -20 dB)
     */
    void setThreshold(float thresholdDb) {
        mThresholdDb.store(thresholdDb, std::memory_order_relaxed);
        mThresholdLinear.store(std::pow(10.0f, thresholdDb / 20.0f), std::memory_order_relaxed);
    }

    /**
     * @brief Set the attack time
     * @param attackMs Attack time in milliseconds (how fast gate opens)
     */
    void setAttackTime(float attackMs) {
        mAttackMs.store(attackMs, std::memory_order_relaxed);
        updateCoefficients();
    }

    /**
     * @brief Set the release time
     * @param releaseMs Release time in milliseconds (how fast gate closes)
     */
    void setReleaseTime(float releaseMs) {
        mReleaseMs.store(releaseMs, std::memory_order_relaxed);
        updateCoefficients();
    }

    /**
     * @brief Set the hysteresis amount
     * @param hysteresisDb Hysteresis in dB (prevents gate chatter)
     */
    void setHysteresis(float hysteresisDb) {
        // Negative hysteresis would put the close threshold ABOVE the open
        // threshold and guarantee chatter.
        mHysteresisDb.store(std::max(0.0f, hysteresisDb), std::memory_order_relaxed);
    }

    /**
     * @brief Process stereo interleaved audio buffer
     * @param buffer Audio buffer (interleaved L/R)
     * @param numFrames Number of frames to process
     */
    void process(float* buffer, int numFrames) {
        float threshold = mThresholdLinear.load(std::memory_order_relaxed);
        float hysteresis = std::pow(10.0f, mHysteresisDb.load(std::memory_order_relaxed) / 20.0f);
        float attackCoeff = mAttackCoeff;
        float releaseCoeff = mReleaseCoeff;

        for (int i = 0; i < numFrames * 2; i += 2) {
            // Detect level (max of L and R)
            float level = std::max(std::abs(buffer[i]), std::abs(buffer[i + 1]));

            // Update envelope follower
            if (level > mEnvelope) {
                mEnvelope = attackCoeff * mEnvelope + (1.0f - attackCoeff) * level;
            } else {
                mEnvelope = releaseCoeff * mEnvelope + (1.0f - releaseCoeff) * level;
            }

            // Gate with hysteresis: the close threshold sits hysteresisDb
            // BELOW the open threshold, i.e. divide the linear amplitude by
            // the linear ratio. (Subtracting the ratio from the amplitude
            // made the close threshold negative, so the gate could never
            // re-close once open.)
            float gateThreshold = mGateOpen ? (threshold / hysteresis) : threshold;

            if (mEnvelope > gateThreshold) {
                mGateOpen = true;
                // Smooth transition to 1.0
                mGain = attackCoeff * mGain + (1.0f - attackCoeff) * 1.0f;
            } else {
                mGateOpen = false;
                // Smooth transition to 0.0
                mGain = releaseCoeff * mGain + (1.0f - releaseCoeff) * 0.0f;
            }

            // Apply gain
            buffer[i] *= mGain;
            buffer[i + 1] *= mGain;
        }
    }

    /**
     * @brief Check if gate is currently open
     * @return true if gate is open (signal passing through)
     */
    bool isOpen() const { return mGateOpen; }

    /**
     * @brief Get current gate gain (for metering)
     * @return Current gain value (0.0 to 1.0)
     */
    float getGain() const { return mGain; }

    /**
     * @brief Reset gate state
     */
    void reset() {
        mEnvelope = 0.0f;
        mGain = 0.0f;
        mGateOpen = false;
    }

private:
    void updateCoefficients() {
        if (mSampleRate <= 0) return;

        float attackMs = mAttackMs.load(std::memory_order_relaxed);
        float releaseMs = mReleaseMs.load(std::memory_order_relaxed);

        mAttackCoeff = std::exp(-1.0f / (attackMs * 0.001f * mSampleRate));
        mReleaseCoeff = std::exp(-1.0f / (releaseMs * 0.001f * mSampleRate));
    }

private:
    int mSampleRate = 48000;

    // Thread-safe parameters
    std::atomic<float> mThresholdDb{-60.0f};
    std::atomic<float> mThresholdLinear{0.001f};
    std::atomic<float> mAttackMs{1.0f};
    std::atomic<float> mReleaseMs{50.0f};
    std::atomic<float> mHysteresisDb{6.0f};

    // Calculated coefficients
    float mAttackCoeff = 0.0f;
    float mReleaseCoeff = 0.0f;

    // Processing state
    float mEnvelope = 0.0f;
    float mGain = 0.0f;
    bool mGateOpen = false;
};
