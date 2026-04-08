#ifndef NOISYPAD_ENVELOPEMODULATOR_H
#define NOISYPAD_ENVELOPEMODULATOR_H

#include "SignalModulator.h"
#include "../dsp/ParameterSmoother.h"
#include <cmath>
#include <atomic>

/**
 * Envelope Modulator - ADSR (Attack, Decay, Sustain, Release).
 *
 * Applies a repeating envelope to the audio signal, shaping its amplitude
 * over time to create instrument-like articulation.
 *
 * In continuous mode, the envelope loops automatically.
 *
 * Parameters:
 * - Attack (0.001 - 2.0s): Time to reach peak
 * - Decay (0.001 - 2.0s): Time to reach sustain level
 * - Sustain (0.0 - 1.0): Sustain level
 * - Release (0.001 - 3.0s): Time to fade out
 */
class EnvelopeModulator : public SignalModulator {
public:
    enum ParamId {
        ATTACK = 0,   // 0.0 - 1.0 -> 1ms - 2000ms
        DECAY = 1,    // 0.0 - 1.0 -> 1ms - 2000ms
        SUSTAIN = 2,  // 0.0 - 1.0
        RELEASE = 3   // 0.0 - 1.0 -> 1ms - 3000ms
    };

    enum class Stage {
        ATTACK,
        DECAY,
        SUSTAIN,
        RELEASE,
        IDLE
    };

    EnvelopeModulator() {
        mAttackSmoother.setSmoothingTime(100.0f, mSampleRate);
        mDecaySmoother.setSmoothingTime(100.0f, mSampleRate);
        mSustainSmoother.setSmoothingTime(100.0f, mSampleRate);
        mReleaseSmoother.setSmoothingTime(100.0f, mSampleRate);
        reset();
    }

    void process(float* buffer, int32_t numFrames) override {
        const float sampleRate = static_cast<float>(mSampleRate.load());

        for (int32_t i = 0; i < numFrames; ++i) {
            // Smooth parameters
            const float attack = mAttackSmoother.process(mAttackTime.load());
            const float decay = mDecaySmoother.process(mDecayTime.load());
            const float sustain = mSustainSmoother.process(mSustainLevel.load());
            const float release = mReleaseSmoother.process(mReleaseTime.load());

            float envelope = 0.0f;

            // State machine for envelope stages
            switch (mStage) {
                case Stage::ATTACK: {
                    mEnvelopePhase += 1.0f / sampleRate;
                    const float attackSec = attack / 1000.0f;

                    if (mEnvelopePhase >= attackSec) {
                        mStage = Stage::DECAY;
                        mEnvelopePhase = 0.0f;
                        envelope = 1.0f;
                    } else {
                        // Exponential attack curve
                        const float t = mEnvelopePhase / attackSec;
                        envelope = t * t * (3.0f - 2.0f * t); // Smoothstep
                    }
                    break;
                }

                case Stage::DECAY: {
                    mEnvelopePhase += 1.0f / sampleRate;
                    const float decaySec = decay / 1000.0f;

                    if (mEnvelopePhase >= decaySec) {
                        mStage = Stage::SUSTAIN;
                        mEnvelopePhase = 0.0f;
                        envelope = sustain;
                    } else {
                        // Exponential decay curve
                        const float t = mEnvelopePhase / decaySec;
                        envelope = 1.0f - (1.0f - sustain) * (t * t * (3.0f - 2.0f * t));
                    }
                    break;
                }

                case Stage::SUSTAIN: {
                    mEnvelopePhase += 1.0f / sampleRate;
                    envelope = sustain;

                    // Auto-retrigger after sustain hold time (1 second)
                    if (mEnvelopePhase >= 1.0f) {
                        mStage = Stage::RELEASE;
                        mEnvelopePhase = 0.0f;
                    }
                    break;
                }

                case Stage::RELEASE: {
                    mEnvelopePhase += 1.0f / sampleRate;
                    const float releaseSec = release / 1000.0f;

                    if (mEnvelopePhase >= releaseSec) {
                        // Loop back to attack
                        mStage = Stage::ATTACK;
                        mEnvelopePhase = 0.0f;
                        envelope = 0.0f;
                    } else {
                        // Exponential release curve
                        const float t = mEnvelopePhase / releaseSec;
                        envelope = sustain * (1.0f - (t * t * (3.0f - 2.0f * t)));
                    }
                    break;
                }

                case Stage::IDLE:
                    envelope = 0.0f;
                    break;
            }

            // Apply envelope to stereo buffer
            const int32_t idx = i * 2;
            buffer[idx] *= envelope;     // Left
            buffer[idx + 1] *= envelope; // Right
        }
    }

    void setParameter(int paramId, float value) override {
        switch (paramId) {
            case ATTACK:
                // Map 0-1 to 1-2000ms (logarithmic)
                mAttackTime.store(1.0f + value * value * 1999.0f);
                break;
            case DECAY:
                // Map 0-1 to 1-2000ms (logarithmic)
                mDecayTime.store(1.0f + value * value * 1999.0f);
                break;
            case SUSTAIN:
                // Direct mapping 0-1
                mSustainLevel.store(std::max(0.0f, std::min(1.0f, value)));
                break;
            case RELEASE:
                // Map 0-1 to 1-3000ms (logarithmic)
                mReleaseTime.store(1.0f + value * value * 2999.0f);
                break;
        }
    }

    void reset() override {
        mStage = Stage::ATTACK;
        mEnvelopePhase = 0.0f;
        mAttackSmoother.reset(mAttackTime.load());
        mDecaySmoother.reset(mDecayTime.load());
        mSustainSmoother.reset(mSustainLevel.load());
        mReleaseSmoother.reset(mReleaseTime.load());
    }

    void setSampleRate(int32_t sampleRate) {
        SignalModulator::setSampleRate(sampleRate);
        mAttackSmoother.setSmoothingTime(100.0f, sampleRate);
        mDecaySmoother.setSmoothingTime(100.0f, sampleRate);
        mSustainSmoother.setSmoothingTime(100.0f, sampleRate);
        mReleaseSmoother.setSmoothingTime(100.0f, sampleRate);
    }

private:
    std::atomic<float> mAttackTime{50.0f};    // ms
    std::atomic<float> mDecayTime{100.0f};    // ms
    std::atomic<float> mSustainLevel{0.7f};   // 0-1
    std::atomic<float> mReleaseTime{200.0f};  // ms

    Stage mStage{Stage::ATTACK};
    float mEnvelopePhase{0.0f};

    ParameterSmoother mAttackSmoother;
    ParameterSmoother mDecaySmoother;
    ParameterSmoother mSustainSmoother;
    ParameterSmoother mReleaseSmoother;
};

#endif // NOISYPAD_ENVELOPEMODULATOR_H
