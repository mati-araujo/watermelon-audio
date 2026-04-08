#ifndef LFO_H
#define LFO_H

#include <atomic>
#include <cmath>
#include <algorithm>
#include "DSPMath.h"

/**
 * @file LFO.h
 * @brief Low Frequency Oscillator for modulation effects
 *
 * Generates low-frequency waveforms (typically 0.01 Hz to 20 Hz) used for:
 * - Chorus/Flanger (delay modulation)
 * - Vibrato (pitch modulation)
 * - Tremolo (amplitude modulation)
 * - Reverb modulation (vintage character)
 *
 * Supports multiple waveforms: sine, triangle, square, sawtooth, random
 *
 * Thread-safe: Rate and depth are atomic, can be updated from any thread
 */
class LFO {
public:
    /**
     * @brief Waveform type
     */
    enum class Waveform {
        SINE,           ///< Smooth sine wave (most common)
        TRIANGLE,       ///< Linear triangle wave
        SQUARE,         ///< Square wave (hard transitions)
        SAWTOOTH,       ///< Sawtooth wave (linear ramp)
        RANDOM,         ///< Sample & hold random (stepped random values)
        RANDOM_SMOOTH   ///< Interpolated random (smooth transitions between random values)
    };

    /**
     * @brief Constructor
     * @param sampleRate Sample rate in Hz
     * @param initialRate LFO rate in Hz (default 1.0 Hz)
     */
    explicit LFO(float sampleRate = 48000.0f, float initialRate = 1.0f);

    /**
     * @brief Set sample rate
     * @param sampleRate Sample rate in Hz
     *
     * Recalculates phase increment. Safe to call anytime.
     */
    void setSampleRate(float sampleRate);

    /**
     * @brief Set LFO rate (frequency)
     * @param rateHz Rate in Hz (typically 0.01 to 20 Hz)
     *
     * Thread-safe: Can be called from any thread
     */
    void setRate(float rateHz);

    /**
     * @brief Get current LFO rate
     * @return Rate in Hz
     */
    float getRate() const {
        return mRateHz.load(std::memory_order_acquire);
    }

    /**
     * @brief Set waveform type
     * @param waveform Waveform to use
     */
    void setWaveform(Waveform waveform);

    /**
     * @brief Get current waveform
     */
    Waveform getWaveform() const { return mWaveform; }

    /**
     * @brief Reset phase to zero
     *
     * Use this to synchronize multiple LFOs or restart modulation
     */
    void reset();

    /**
     * @brief Set phase offset
     * @param phaseOffset Phase offset in radians (0 to 2π)
     *
     * Useful for creating stereo LFOs with different phases:
     * - lfoL.setPhaseOffset(0)
     * - lfoR.setPhaseOffset(PI / 2)  // 90° offset
     */
    void setPhaseOffset(float phaseOffset);

    /**
     * @brief Get next sample from LFO
     * @return LFO output value in range [-1.0, 1.0]
     *
     * RT-safe: O(1) operation, call once per sample
     */
    float process();

    /**
     * @brief Get next sample scaled to custom range
     * @param min Minimum output value
     * @param max Maximum output value
     * @return LFO output value in range [min, max]
     *
     * Convenience method: output = min + (lfo + 1) / 2 * (max - min)
     */
    float processScaled(float min, float max);

    /**
     * @brief Get next sample as unipolar (0 to 1) instead of bipolar (-1 to 1)
     * @return LFO output value in range [0.0, 1.0]
     */
    float processUnipolar();

    /**
     * @brief Get current phase
     * @return Phase in radians [0, 2π)
     */
    float getPhase() const { return mPhase; }

    /**
     * @brief Sync to external phase (for synchronized LFOs)
     * @param externalPhase Phase to sync to
     */
    void syncToPhase(float externalPhase);

private:
    float mSampleRate{48000.0f};         ///< Sample rate
    std::atomic<float> mRateHz{1.0f};    ///< LFO rate in Hz (atomic for thread-safety)
    float mPhase{0.0f};                  ///< Current phase [0, 2π)
    float mPhaseIncrement{0.0f};         ///< Phase increment per sample
    float mPhaseOffset{0.0f};            ///< Phase offset for stereo
    Waveform mWaveform{Waveform::SINE};  ///< Current waveform

    // Random waveform state
    float mRandomValue{0.0f};            ///< Current random value (S&H)
    float mRandomPhase{0.0f};            ///< Phase for random update

    // Random smooth waveform state
    float mRandomPrev{0.0f};             ///< Previous random target
    float mRandomNext{0.0f};             ///< Next random target
    float mRandomSmoothPhase{0.0f};      ///< Interpolation phase [0, 1)

    /**
     * @brief Update phase increment based on current rate
     */
    void updatePhaseIncrement();

    /**
     * @brief Generate output for current waveform
     * @param phase Current phase [0, 2π)
     * @return Waveform output [-1, 1]
     */
    float generateWaveform(float phase);

    /**
     * @brief Generate sine wave
     */
    float generateSine(float phase);

    /**
     * @brief Generate triangle wave
     */
    float generateTriangle(float phase);

    /**
     * @brief Generate square wave
     */
    float generateSquare(float phase);

    /**
     * @brief Generate sawtooth wave
     */
    float generateSawtooth(float phase);

    /**
     * @brief Generate random wave (sample & hold)
     */
    float generateRandom();

    /**
     * @brief Generate smooth random wave (linear interpolation between random values)
     */
    float generateRandomSmooth();

    /**
     * @brief Random number generator [-1, 1]
     */
    float randomFloat();
};

#endif // LFO_H
