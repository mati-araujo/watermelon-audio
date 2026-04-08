#pragma once

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @class StateVariableFilter
 * @brief Zero-delay feedback State Variable Filter (SVF)
 *
 * Provides simultaneous lowpass, highpass, and bandpass outputs.
 * Numerically stable and easy to modulate (cutoff/resonance can change
 * per-sample without artifacts).
 *
 * Based on Andrew Simper's SVF design (Cytomic).
 *
 * RT-Safety: No allocations, no branches that depend on history length.
 */
class StateVariableFilter {
public:
    enum class Mode {
        LOWPASS = 0,
        HIGHPASS = 1,
        BANDPASS = 2
    };

    StateVariableFilter() = default;

    void prepare(float sampleRate) {
        mSampleRate = sampleRate;
        reset();
    }

    void reset() {
        mIc1eq = 0.0f;
        mIc2eq = 0.0f;
    }

    /**
     * @brief Set filter cutoff frequency
     * @param hz Cutoff in Hz (20-20000)
     */
    void setCutoff(float hz) {
        hz = std::clamp(hz, 20.0f, mSampleRate * 0.49f);
        mG = std::tan(static_cast<float>(M_PI) * hz / mSampleRate);
    }

    /**
     * @brief Set filter resonance
     * @param q Resonance 0.0-1.0 (maps to Q factor 0.5-20)
     */
    void setResonance(float q) {
        // Map 0-1 to Q range: 0.5 (no resonance) to 20 (high resonance)
        float qFactor = 0.5f + q * 19.5f;
        mK = 1.0f / qFactor;
    }

    /**
     * @brief Set filter mode
     */
    void setMode(Mode mode) {
        mMode = mode;
    }

    /**
     * @brief Process a single sample
     * @param input Input sample
     * @return Filtered output (mode-dependent)
     */
    float process(float input) {
        // Cytomic SVF (trapezoidal integration)
        float v3 = input - mIc2eq;
        float v1 = mA1 * mIc1eq + mA2 * v3;
        float v2 = mIc2eq + mA2 * mIc1eq + mA3 * v3;

        mIc1eq = 2.0f * v1 - mIc1eq;
        mIc2eq = 2.0f * v2 - mIc2eq;

        // Recalculate coefficients (allows per-sample modulation)
        updateCoefficients();

        switch (mMode) {
            case Mode::LOWPASS:  return v2;
            case Mode::HIGHPASS: return input - mK * v1 - v2;
            case Mode::BANDPASS: return v1;
            default: return v2;
        }
    }

    /**
     * @brief Process a stereo interleaved buffer in-place
     * @param buffer Stereo interleaved buffer [L0,R0,L1,R1,...]
     * @param numFrames Number of frames
     *
     * Note: Uses single filter state, so L and R share the same filter.
     * For true stereo, use two instances.
     */
    void processBlock(float* buffer, int numFrames) {
        for (int i = 0; i < numFrames; ++i) {
            // Process left channel only (mono filter before panning)
            buffer[i * 2] = process(buffer[i * 2]);
            buffer[i * 2 + 1] = process(buffer[i * 2 + 1]);
        }
    }

private:
    float mSampleRate = 48000.0f;
    float mG = 0.0f;     // tan(π × fc / fs)
    float mK = 1.0f;     // 1/Q (damping)

    // SVF coefficients
    float mA1 = 0.0f;
    float mA2 = 0.0f;
    float mA3 = 0.0f;

    // State
    float mIc1eq = 0.0f;
    float mIc2eq = 0.0f;

    Mode mMode = Mode::LOWPASS;

    void updateCoefficients() {
        mA1 = 1.0f / (1.0f + mG * (mG + mK));
        mA2 = mG * mA1;
        mA3 = mG * mA2;
    }
};
