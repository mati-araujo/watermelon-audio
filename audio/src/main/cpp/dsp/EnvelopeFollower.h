#ifndef ENVELOPEFOLLOWER_H
#define ENVELOPEFOLLOWER_H

#include <atomic>
#include <cmath>
#include <algorithm>
#include "DSPMath.h"

/**
 * @file EnvelopeFollower.h
 * @brief Peak/RMS envelope follower for vocoder band analysis
 *
 * Extracts the amplitude envelope from an audio signal using
 * asymmetric attack/release smoothing. Essential for vocoder
 * band-level extraction and dynamics processing.
 *
 * Thread-safe: Attack/release coefficients can be updated from UI thread
 * while audio thread processes samples.
 *
 * Usage:
 *   EnvelopeFollower follower;
 *   follower.prepare(48000);
 *   follower.setAttack(5.0f);   // 5ms attack
 *   follower.setRelease(50.0f); // 50ms release
 *
 *   float envelope = follower.process(audioSample);
 */
class EnvelopeFollower {
public:
    /**
     * @brief Detection mode
     */
    enum class Mode {
        PEAK,   ///< Track peak amplitude (faster, more dynamic)
        RMS     ///< Track RMS power (smoother, more average-like)
    };

    /**
     * @brief Default constructor
     */
    EnvelopeFollower() = default;

    /**
     * @brief Copy constructor
     * Atomics are not copyable, so we load their values manually
     */
    EnvelopeFollower(const EnvelopeFollower& other)
        : mSampleRate(other.mSampleRate)
        , mAttackMs(other.mAttackMs.load(std::memory_order_relaxed))
        , mReleaseMs(other.mReleaseMs.load(std::memory_order_relaxed))
        , mAttackCoeff(other.mAttackCoeff.load(std::memory_order_relaxed))
        , mReleaseCoeff(other.mReleaseCoeff.load(std::memory_order_relaxed))
        , mEnvelope(other.mEnvelope)
        , mMode(other.mMode) {}

    /**
     * @brief Move constructor
     */
    EnvelopeFollower(EnvelopeFollower&& other) noexcept
        : mSampleRate(other.mSampleRate)
        , mAttackMs(other.mAttackMs.load(std::memory_order_relaxed))
        , mReleaseMs(other.mReleaseMs.load(std::memory_order_relaxed))
        , mAttackCoeff(other.mAttackCoeff.load(std::memory_order_relaxed))
        , mReleaseCoeff(other.mReleaseCoeff.load(std::memory_order_relaxed))
        , mEnvelope(other.mEnvelope)
        , mMode(other.mMode) {
        other.mEnvelope = 0.0f;
    }

    /**
     * @brief Copy assignment operator
     */
    EnvelopeFollower& operator=(const EnvelopeFollower& other) {
        if (this != &other) {
            mSampleRate = other.mSampleRate;
            mAttackMs.store(other.mAttackMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mReleaseMs.store(other.mReleaseMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mAttackCoeff.store(other.mAttackCoeff.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mReleaseCoeff.store(other.mReleaseCoeff.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mEnvelope = other.mEnvelope;
            mMode = other.mMode;
        }
        return *this;
    }

    /**
     * @brief Move assignment operator
     */
    EnvelopeFollower& operator=(EnvelopeFollower&& other) noexcept {
        if (this != &other) {
            mSampleRate = other.mSampleRate;
            mAttackMs.store(other.mAttackMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mReleaseMs.store(other.mReleaseMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mAttackCoeff.store(other.mAttackCoeff.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mReleaseCoeff.store(other.mReleaseCoeff.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mEnvelope = other.mEnvelope;
            mMode = other.mMode;
            other.mEnvelope = 0.0f;
        }
        return *this;
    }

    /**
     * @brief Prepare the envelope follower for processing
     * @param sampleRate Sample rate in Hz
     */
    void prepare(float sampleRate) {
        mSampleRate = sampleRate;
        updateCoefficients();
    }

    /**
     * @brief Set attack time
     * @param attackMs Attack time in milliseconds (0.1 to 100ms typical)
     *
     * Attack determines how quickly the envelope rises when signal increases.
     * Fast attack (< 5ms): Tight, punchy response
     * Slow attack (> 20ms): Smooth, averaged response
     */
    void setAttack(float attackMs) {
        attackMs = std::clamp(attackMs, 0.1f, 500.0f);
        mAttackMs.store(attackMs, std::memory_order_relaxed);
        updateCoefficients();
    }

    /**
     * @brief Set release time
     * @param releaseMs Release time in milliseconds (1 to 500ms typical)
     *
     * Release determines how quickly the envelope falls when signal decreases.
     * Fast release (< 20ms): Punchy, but can cause pumping
     * Slow release (> 100ms): Smooth, but may miss transients
     */
    void setRelease(float releaseMs) {
        releaseMs = std::clamp(releaseMs, 0.1f, 2000.0f);
        mReleaseMs.store(releaseMs, std::memory_order_relaxed);
        updateCoefficients();
    }

    /**
     * @brief Set detection mode
     * @param mode PEAK or RMS detection
     */
    void setMode(Mode mode) {
        mMode = mode;
    }

    /**
     * @brief Process a single sample and return envelope value
     * @param input Input sample
     * @return Current envelope value (always positive)
     *
     * RT-safe: Uses atomic loads for coefficients
     */
    float process(float input) {
        // Get absolute value (peak) or squared value (RMS)
        float rectified = (mMode == Mode::RMS) ? input * input : std::abs(input);

        // Asymmetric smoothing (attack/release)
        float attackCoeff = mAttackCoeff.load(std::memory_order_relaxed);
        float releaseCoeff = mReleaseCoeff.load(std::memory_order_relaxed);

        if (rectified > mEnvelope) {
            // Rising (attack)
            mEnvelope += attackCoeff * (rectified - mEnvelope);
        } else {
            // Falling (release)
            mEnvelope += releaseCoeff * (rectified - mEnvelope);
        }

        // Prevent denormals
        if (mEnvelope < DSPMath::EPSILON) {
            mEnvelope = 0.0f;
        }

        // Return envelope (sqrt for RMS to convert power to amplitude)
        return (mMode == Mode::RMS) ? std::sqrt(mEnvelope) : mEnvelope;
    }

    /**
     * @brief Process a block of samples
     * @param input Input buffer
     * @param envelope Output envelope buffer
     * @param numSamples Number of samples to process
     *
     * RT-safe: Coefficients loaded once at start of block
     */
    void processBlock(const float* input, float* envelope, int numSamples) {
        float attackCoeff = mAttackCoeff.load(std::memory_order_acquire);
        float releaseCoeff = mReleaseCoeff.load(std::memory_order_acquire);
        bool isRMS = (mMode == Mode::RMS);

        for (int i = 0; i < numSamples; ++i) {
            float sample = input[i];
            float rectified = isRMS ? sample * sample : std::abs(sample);

            if (rectified > mEnvelope) {
                mEnvelope += attackCoeff * (rectified - mEnvelope);
            } else {
                mEnvelope += releaseCoeff * (rectified - mEnvelope);
            }

            if (mEnvelope < DSPMath::EPSILON) {
                mEnvelope = 0.0f;
            }

            envelope[i] = isRMS ? std::sqrt(mEnvelope) : mEnvelope;
        }
    }

    /**
     * @brief Get current envelope value without processing new input
     * @return Current envelope value
     */
    float getCurrentValue() const {
        return (mMode == Mode::RMS) ? std::sqrt(mEnvelope) : mEnvelope;
    }

    /**
     * @brief Reset envelope state
     *
     * Call when starting/stopping audio to avoid clicks
     */
    void reset() {
        mEnvelope = 0.0f;
    }

    /**
     * @brief Get attack time
     * @return Attack time in milliseconds
     */
    float getAttack() const {
        return mAttackMs.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get release time
     * @return Release time in milliseconds
     */
    float getRelease() const {
        return mReleaseMs.load(std::memory_order_relaxed);
    }

private:
    float mSampleRate{48000.0f};

    // Parameters (thread-safe)
    std::atomic<float> mAttackMs{5.0f};
    std::atomic<float> mReleaseMs{50.0f};

    // Coefficients (updated when parameters change)
    std::atomic<float> mAttackCoeff{0.0f};
    std::atomic<float> mReleaseCoeff{0.0f};

    // State
    float mEnvelope{0.0f};
    Mode mMode{Mode::PEAK};

    /**
     * @brief Update filter coefficients from parameters
     */
    void updateCoefficients() {
        float attackMs = mAttackMs.load(std::memory_order_relaxed);
        float releaseMs = mReleaseMs.load(std::memory_order_relaxed);

        // One-pole smoothing coefficient: 1 - e^(-1/(time_constant_samples))
        mAttackCoeff.store(DSPMath::onePoleCoefficient(attackMs, mSampleRate),
                           std::memory_order_release);
        mReleaseCoeff.store(DSPMath::onePoleCoefficient(releaseMs, mSampleRate),
                            std::memory_order_release);
    }
};

#endif // ENVELOPEFOLLOWER_H
