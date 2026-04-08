#ifndef NOISYPAD_GATEMODULATOR_H
#define NOISYPAD_GATEMODULATOR_H

#include "SignalModulator.h"
#include "../dsp/ParameterSmoother.h"
#include <cmath>
#include <array>

/**
 * Gate Modulator - Rhythmic pattern-based gating.
 *
 * Applies repeating on/off patterns to the audio signal,
 * creating rhythmic and sequenced effects.
 *
 * Parameters:
 * - Rate (0.5 - 16 Hz): Pattern repetition speed
 * - Pattern (0-15): Predefined gate patterns
 * - Gate Time (0.1 - 1.0): Percentage of time gate is open
 */
class GateModulator : public SignalModulator {
public:
    enum ParamId {
        RATE = 0,      // Hz: 0.5 - 16
        PATTERN = 1,   // 0-15: pattern selection
        GATE_TIME = 2  // 0.1 - 1.0
    };

    GateModulator() {
        mRateSmoother.setSmoothingTime(100.0f, mSampleRate);
        mGateTimeSmoother.setSmoothingTime(50.0f, mSampleRate);
        initializePatterns();
        reset();
    }

    void process(float* buffer, int32_t numFrames) override {
        const float sampleRate = static_cast<float>(mSampleRate.load());

        for (int32_t i = 0; i < numFrames; ++i) {
            // Smooth parameters
            const float rate = mRateSmoother.process(mRate.load());
            const float gateTime = mGateTimeSmoother.process(mGateTime.load());
            const int patternIndex = mPatternIndex.load();

            // Calculate step duration
            const float stepDuration = 1.0f / (rate * mPatterns[patternIndex].size());
            const float stepDurationSamples = stepDuration * sampleRate;

            // Determine current step in pattern
            const int currentStep = static_cast<int>(mPhase / stepDurationSamples) % mPatterns[patternIndex].size();
            const bool gateOpen = mPatterns[patternIndex][currentStep];

            // Calculate gate envelope
            const float stepPhase = std::fmod(mPhase, stepDurationSamples) / stepDurationSamples;

            float gate = 0.0f;
            if (gateOpen) {
                if (stepPhase < gateTime) {
                    // Gate is open
                    // Apply short fade in/out to avoid clicks
                    const float fadeTime = 0.05f; // 5% fade time

                    if (stepPhase < fadeTime) {
                        // Fade in
                        gate = stepPhase / fadeTime;
                    } else if (stepPhase > gateTime - fadeTime && gateTime > fadeTime) {
                        // Fade out
                        gate = (gateTime - stepPhase) / fadeTime;
                    } else {
                        // Fully open
                        gate = 1.0f;
                    }

                    // Smoothstep curve
                    gate = gate * gate * (3.0f - 2.0f * gate);
                }
            }

            // Apply gate to stereo buffer
            const int32_t idx = i * 2;
            buffer[idx] *= gate;     // Left
            buffer[idx + 1] *= gate; // Right

            // Advance phase
            mPhase += 1.0f;
            const float patternDuration = stepDurationSamples * mPatterns[patternIndex].size();
            if (mPhase >= patternDuration) {
                mPhase -= patternDuration;
            }
        }
    }

    void setParameter(int paramId, float value) override {
        switch (paramId) {
            case RATE:
                // Map 0-1 to 0.5-16 Hz
                mRate.store(0.5f + value * 15.5f);
                break;
            case PATTERN:
                // Map 0-1 to 0-15 (16 patterns)
                mPatternIndex.store(static_cast<int>(value * 15.99f) % 16);
                break;
            case GATE_TIME:
                // Map 0-1 to 0.1-1.0
                mGateTime.store(0.1f + value * 0.9f);
                break;
        }
    }

    void reset() override {
        mPhase = 0.0f;
        mRateSmoother.reset(mRate.load());
        mGateTimeSmoother.reset(mGateTime.load());
    }

    void setSampleRate(int32_t sampleRate) {
        SignalModulator::setSampleRate(sampleRate);
        mRateSmoother.setSmoothingTime(100.0f, sampleRate);
        mGateTimeSmoother.setSmoothingTime(50.0f, sampleRate);
    }

private:
    void initializePatterns() {
        // Define 16 rhythmic gate patterns (1 = gate open, 0 = gate closed)
        mPatterns[0] = {1, 0, 1, 0};                    // Basic on-off
        mPatterns[1] = {1, 1, 0, 0};                    // 50% duty
        mPatterns[2] = {1, 0, 0, 0};                    // Sparse
        mPatterns[3] = {1, 1, 1, 0};                    // 75% on
        mPatterns[4] = {1, 0, 1, 1, 0, 1, 0, 0};        // Funky rhythm
        mPatterns[5] = {1, 1, 0, 1, 0, 1, 1, 0};        // Syncopated
        mPatterns[6] = {1, 0, 0, 1, 0, 0, 1, 0};        // Sparse triplet
        mPatterns[7] = {1, 1, 1, 1, 0, 0, 0, 0};        // Half bar
        mPatterns[8] = {1, 0, 1, 0, 1, 0, 1, 1};        // Almost continuous
        mPatterns[9] = {1, 0, 0, 0, 1, 0, 0, 0};        // Very sparse
        mPatterns[10] = {1, 1, 0, 1, 1, 0, 1, 0};       // Complex rhythm
        mPatterns[11] = {1, 0, 1, 1, 1, 0, 1, 1};       // Asymmetric
        mPatterns[12] = {1, 1, 1, 0, 1, 0, 0, 0};       // Front-loaded
        mPatterns[13] = {1, 0, 0, 1, 1, 0, 0, 1};       // Symmetric
        mPatterns[14] = {1, 1, 1, 1, 1, 1, 0, 0};       // Long sustain
        mPatterns[15] = {1, 0, 1, 0, 0, 1, 0, 1, 0, 0}; // Extended pattern
    }

    std::atomic<float> mRate{4.0f};         // Hz
    std::atomic<int> mPatternIndex{0};      // 0-15
    std::atomic<float> mGateTime{0.5f};     // 0.1-1.0

    float mPhase{0.0f};

    // Gate patterns (various rhythmic sequences)
    std::array<std::vector<bool>, 16> mPatterns;

    ParameterSmoother mRateSmoother;
    ParameterSmoother mGateTimeSmoother;
};

#endif // NOISYPAD_GATEMODULATOR_H
