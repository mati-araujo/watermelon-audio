#pragma once

#include <cmath>
#include <atomic>
#include <algorithm>

/**
 * @class LevelMeter
 * @brief Professional stereo level meter with peak hold and RMS
 *
 * Features:
 * - Peak level with ballistics (attack/release)
 * - Peak hold indicator
 * - RMS level calculation
 * - Clipping detection with hold
 * - Thread-safe level reading for UI
 */
class LevelMeter {
public:
    /**
     * @brief Prepare the meter for processing
     * @param sampleRate Sample rate in Hz
     */
    void prepare(int sampleRate) {
        mSampleRate = sampleRate;
        updateCoefficients();
    }

    /**
     * @brief Set attack time for peak detection
     * @param attackMs Attack time in milliseconds
     */
    void setAttackTime(float attackMs) {
        mAttackMs = attackMs;
        updateCoefficients();
    }

    /**
     * @brief Set release time for peak decay
     * @param releaseMs Release time in milliseconds
     */
    void setReleaseTime(float releaseMs) {
        mReleaseMs = releaseMs;
        updateCoefficients();
    }

    /**
     * @brief Set peak hold time
     * @param holdMs Hold time in milliseconds
     */
    void setPeakHoldTime(float holdMs) {
        mPeakHoldSamples = static_cast<int>(holdMs * 0.001f * mSampleRate);
    }

    /**
     * @brief Process stereo interleaved audio buffer
     * @param buffer Audio buffer (interleaved L/R)
     * @param numFrames Number of frames to process
     */
    void process(const float* buffer, int numFrames) {
        float peakL = 0.0f;
        float peakR = 0.0f;
        float sumL = 0.0f;
        float sumR = 0.0f;

        // Find peak and sum for RMS
        for (int i = 0; i < numFrames * 2; i += 2) {
            float absL = std::abs(buffer[i]);
            float absR = std::abs(buffer[i + 1]);

            peakL = std::max(peakL, absL);
            peakR = std::max(peakR, absR);

            sumL += buffer[i] * buffer[i];
            sumR += buffer[i + 1] * buffer[i + 1];
        }

        // Update peak with ballistics - Left channel
        if (peakL > mPeakL) {
            mPeakL = mAttackCoeff * mPeakL + (1.0f - mAttackCoeff) * peakL;
            mPeakHoldCounterL = mPeakHoldSamples;
            mHeldPeakL = std::max(mHeldPeakL, mPeakL);
        } else {
            if (mPeakHoldCounterL > 0) {
                mPeakHoldCounterL -= numFrames;
            } else {
                mPeakL = mReleaseCoeff * mPeakL;
                mHeldPeakL = mPeakL;
            }
        }

        // Update peak with ballistics - Right channel
        if (peakR > mPeakR) {
            mPeakR = mAttackCoeff * mPeakR + (1.0f - mAttackCoeff) * peakR;
            mPeakHoldCounterR = mPeakHoldSamples;
            mHeldPeakR = std::max(mHeldPeakR, mPeakR);
        } else {
            if (mPeakHoldCounterR > 0) {
                mPeakHoldCounterR -= numFrames;
            } else {
                mPeakR = mReleaseCoeff * mPeakR;
                mHeldPeakR = mPeakR;
            }
        }

        // RMS calculation
        mRmsL = std::sqrt(sumL / numFrames);
        mRmsR = std::sqrt(sumR / numFrames);

        // Clipping detection
        mClippingL = peakL >= 1.0f;
        mClippingR = peakR >= 1.0f;
        if (mClippingL || mClippingR) {
            mClipHoldCounter = mPeakHoldSamples * 2;  // Hold clip indicator longer
        } else if (mClipHoldCounter > 0) {
            mClipHoldCounter -= numFrames;
        }

        // Update atomics for thread-safe UI reading
        mAtomicPeakL.store(mPeakL, std::memory_order_relaxed);
        mAtomicPeakR.store(mPeakR, std::memory_order_relaxed);
        mAtomicRmsL.store(mRmsL, std::memory_order_relaxed);
        mAtomicRmsR.store(mRmsR, std::memory_order_relaxed);
        mAtomicClipping.store(mClipHoldCounter > 0, std::memory_order_relaxed);
    }

    // Thread-safe getters for UI
    float getPeakL() const { return mAtomicPeakL.load(std::memory_order_relaxed); }
    float getPeakR() const { return mAtomicPeakR.load(std::memory_order_relaxed); }
    float getRmsL() const { return mAtomicRmsL.load(std::memory_order_relaxed); }
    float getRmsR() const { return mAtomicRmsR.load(std::memory_order_relaxed); }
    bool isClipping() const { return mAtomicClipping.load(std::memory_order_relaxed); }

    /**
     * @brief Get peak level in dB
     * @param channel 0 for left, 1 for right
     * @return Peak level in dB (-120 to 0)
     */
    float getPeakDb(int channel) const {
        float peak = (channel == 0) ? getPeakL() : getPeakR();
        return (peak > 0.0f) ? 20.0f * std::log10(peak) : -120.0f;
    }

    /**
     * @brief Get RMS level in dB
     * @param channel 0 for left, 1 for right
     * @return RMS level in dB (-120 to 0)
     */
    float getRmsDb(int channel) const {
        float rms = (channel == 0) ? getRmsL() : getRmsR();
        return (rms > 0.0f) ? 20.0f * std::log10(rms) : -120.0f;
    }

    /**
     * @brief Reset all meters
     */
    void reset() {
        mPeakL = mPeakR = 0.0f;
        mRmsL = mRmsR = 0.0f;
        mHeldPeakL = mHeldPeakR = 0.0f;
        mPeakHoldCounterL = mPeakHoldCounterR = 0;
        mClipHoldCounter = 0;
        mClippingL = mClippingR = false;

        mAtomicPeakL.store(0.0f, std::memory_order_relaxed);
        mAtomicPeakR.store(0.0f, std::memory_order_relaxed);
        mAtomicRmsL.store(0.0f, std::memory_order_relaxed);
        mAtomicRmsR.store(0.0f, std::memory_order_relaxed);
        mAtomicClipping.store(false, std::memory_order_relaxed);
    }

private:
    void updateCoefficients() {
        mAttackCoeff = std::exp(-1.0f / (mAttackMs * 0.001f * mSampleRate));
        mReleaseCoeff = std::exp(-1.0f / (mReleaseMs * 0.001f * mSampleRate));
    }

private:
    int mSampleRate = 48000;
    float mAttackMs = 0.1f;      // Very fast attack for peak detection
    float mReleaseMs = 300.0f;   // Slow release for visual smoothness
    int mPeakHoldSamples = 48000;  // 1 second default

    float mAttackCoeff = 0.0f;
    float mReleaseCoeff = 0.0f;

    // Processing state
    float mPeakL = 0.0f;
    float mPeakR = 0.0f;
    float mRmsL = 0.0f;
    float mRmsR = 0.0f;
    float mHeldPeakL = 0.0f;
    float mHeldPeakR = 0.0f;

    int mPeakHoldCounterL = 0;
    int mPeakHoldCounterR = 0;
    int mClipHoldCounter = 0;
    bool mClippingL = false;
    bool mClippingR = false;

    // Thread-safe atomics for UI
    std::atomic<float> mAtomicPeakL{0.0f};
    std::atomic<float> mAtomicPeakR{0.0f};
    std::atomic<float> mAtomicRmsL{0.0f};
    std::atomic<float> mAtomicRmsR{0.0f};
    std::atomic<bool> mAtomicClipping{false};
};
