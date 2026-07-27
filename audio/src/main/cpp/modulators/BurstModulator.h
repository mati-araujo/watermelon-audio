#ifndef NOISYPAD_BURSTMODULATOR_H
#define NOISYPAD_BURSTMODULATOR_H

#include "SignalModulator.h"
#include "../dsp/ParameterSmoother.h"
#include <cmath>

/**
 * Burst Modulator - Generates periodic bursts of audio signal.
 *
 * Creates a gating effect where the audio signal is turned on and off
 * at a regular rate, useful for rhythmic effects, stuttering, and percussive sounds.
 *
 * Parameters:
 * - Rate (0.1 - 20 Hz): How frequently bursts occur
 * - Duration (0.01 - 1.0): Percentage of cycle where signal is on
 * - Shape (0.0 - 1.0): Envelope shape (0=hard gate, 1=smooth fade)
 */
class BurstModulator : public SignalModulator {
public:
    enum ParamId {
        RATE = 0,      // Hz: 0.1 - 20
        DURATION = 1,  // 0.0 - 1.0: percentage of cycle
        SHAPE = 2      // 0.0 - 1.0: gate shape (0=hard, 1=smooth)
    };

    BurstModulator() {
        mRateSmoother.setSmoothingTime(50.0f, mSampleRate);
        mDurationSmoother.setSmoothingTime(50.0f, mSampleRate);
        mShapeSmoother.setSmoothingTime(50.0f, mSampleRate);
        reset();
    }

    void process(float* buffer, int32_t numFrames) override {
        const float sampleRate = static_cast<float>(mSampleRate.load());

        for (int32_t i = 0; i < numFrames; ++i) {
            // Smooth parameters
            const float rate = mRateSmoother.process(mRate.load());
            const float duration = mDurationSmoother.process(mDuration.load());
            const float shape = mShapeSmoother.process(mShape.load());

            // Calculate gate value based on phase
            const float cycleTime = 1.0f / std::max(rate, 0.01f);
            const float gateOnTime = cycleTime * duration;
            const float phaseTime = mPhase / sampleRate;

            float gate = 0.0f;

            if (phaseTime < gateOnTime) {
                // Signal is on. El shaping de abajo trabaja con `phaseTime` y
                // `fadeTime` directamente; la posicion normalizada se calculaba
                // y no se leia.

                // Apply shaping
                if (shape < 0.01f) {
                    // Hard gate
                    gate = 1.0f;
                } else {
                    // Smooth envelope
                    const float fadeTime = gateOnTime * shape * 0.5f;

                    if (phaseTime < fadeTime) {
                        // Attack
                        gate = phaseTime / fadeTime;
                    } else if (phaseTime > gateOnTime - fadeTime) {
                        // Release
                        gate = (gateOnTime - phaseTime) / fadeTime;
                    } else {
                        // Sustain
                        gate = 1.0f;
                    }

                    // Smooth curve
                    gate = gate * gate * (3.0f - 2.0f * gate); // Smoothstep
                }
            }

            // Apply gate to stereo buffer
            const int32_t idx = i * 2;
            buffer[idx] *= gate;     // Left
            buffer[idx + 1] *= gate; // Right

            // Advance phase
            mPhase += 1.0f;
            if (mPhase >= cycleTime * sampleRate) {
                mPhase -= cycleTime * sampleRate;
            }
        }
    }

    void setParameter(int paramId, float value) override {
        switch (paramId) {
            case RATE:
                // Map 0-1 to 0.1-20 Hz (logarithmic)
                mRate.store(0.1f + value * value * 19.9f);
                break;
            case DURATION:
                // Direct mapping 0-1
                mDuration.store(std::max(0.01f, std::min(1.0f, value)));
                break;
            case SHAPE:
                // Direct mapping 0-1
                mShape.store(std::max(0.0f, std::min(1.0f, value)));
                break;
        }
    }

    void reset() override {
        mPhase = 0.0f;
        mRateSmoother.reset(mRate.load());
        mDurationSmoother.reset(mDuration.load());
        mShapeSmoother.reset(mShape.load());
    }

    void setSampleRate(int32_t sampleRate) {
        SignalModulator::setSampleRate(sampleRate);
        mRateSmoother.setSmoothingTime(50.0f, sampleRate);
        mDurationSmoother.setSmoothingTime(50.0f, sampleRate);
        mShapeSmoother.setSmoothingTime(50.0f, sampleRate);
    }

private:
    std::atomic<float> mRate{4.0f};        // Hz
    std::atomic<float> mDuration{0.5f};    // 0-1
    std::atomic<float> mShape{0.3f};       // 0-1

    float mPhase{0.0f};

    ParameterSmoother mRateSmoother;
    ParameterSmoother mDurationSmoother;
    ParameterSmoother mShapeSmoother;
};

#endif // NOISYPAD_BURSTMODULATOR_H
