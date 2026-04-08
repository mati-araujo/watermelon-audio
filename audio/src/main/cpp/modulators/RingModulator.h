#ifndef NOISYPAD_RINGMODULATOR_H
#define NOISYPAD_RINGMODULATOR_H

#include "SignalModulator.h"
#include "../dsp/ParameterSmoother.h"
#include <cmath>

/**
 * Ring Modulator.
 *
 * Multiplies the audio signal with a carrier sine wave,
 * creating inharmonic, metallic, and robotic sounds.
 *
 * Classic effect used for sci-fi sounds, Daleks, and bell tones.
 *
 * Parameters:
 * - Carrier Frequency (20 - 5000 Hz): Modulation frequency
 * - Mix (0.0 - 1.0): Dry/wet balance
 * - Feedback (0.0 - 0.9): Internal feedback for harsher tones
 */
class RingModulator : public SignalModulator {
public:
    enum ParamId {
        CARRIER_FREQ = 0,  // Hz: 20 - 5000
        MIX = 1,           // 0.0 - 1.0
        FEEDBACK = 2       // 0.0 - 0.9
    };

    RingModulator() {
        mCarrierFreqSmoother.setSmoothingTime(50.0f, mSampleRate);
        mMixSmoother.setSmoothingTime(50.0f, mSampleRate);
        mFeedbackSmoother.setSmoothingTime(50.0f, mSampleRate);
        reset();
    }

    void process(float* buffer, int32_t numFrames) override {
        const float sampleRate = static_cast<float>(mSampleRate.load());
        const float twoPi = 2.0f * 3.14159265359f;

        for (int32_t i = 0; i < numFrames; ++i) {
            // Smooth parameters
            const float carrierFreq = mCarrierFreqSmoother.process(mCarrierFreq.load());
            const float mix = mMixSmoother.process(mMix.load());
            const float feedback = mFeedbackSmoother.process(mFeedback.load());

            // Generate carrier wave (sine)
            const float carrier = std::sin(mCarrierPhase);

            // Apply ring modulation (multiplication)
            const int32_t idx = i * 2;
            const float leftDry = buffer[idx];
            const float rightDry = buffer[idx + 1];

            // Add feedback for harsher tones
            const float leftWithFeedback = leftDry + (mLastLeft * feedback);
            const float rightWithFeedback = rightDry + (mLastRight * feedback);

            // Ring modulate
            const float leftWet = leftWithFeedback * carrier;
            const float rightWet = rightWithFeedback * carrier;

            // Mix dry and wet
            buffer[idx] = leftDry * (1.0f - mix) + leftWet * mix;
            buffer[idx + 1] = rightDry * (1.0f - mix) + rightWet * mix;

            // Store for feedback
            mLastLeft = leftWet * 0.5f;
            mLastRight = rightWet * 0.5f;

            // Advance carrier phase
            mCarrierPhase += twoPi * carrierFreq / sampleRate;
            if (mCarrierPhase >= twoPi) {
                mCarrierPhase -= twoPi;
            }
        }
    }

    void setParameter(int paramId, float value) override {
        switch (paramId) {
            case CARRIER_FREQ:
                // Map 0-1 to 20-5000 Hz (logarithmic)
                mCarrierFreq.store(20.0f * std::pow(250.0f, value));
                break;
            case MIX:
                // Direct mapping 0-1
                mMix.store(std::max(0.0f, std::min(1.0f, value)));
                break;
            case FEEDBACK:
                // Direct mapping 0-0.9
                mFeedback.store(std::max(0.0f, std::min(0.9f, value)));
                break;
        }
    }

    void reset() override {
        mCarrierPhase = 0.0f;
        mLastLeft = 0.0f;
        mLastRight = 0.0f;
        mCarrierFreqSmoother.reset(mCarrierFreq.load());
        mMixSmoother.reset(mMix.load());
        mFeedbackSmoother.reset(mFeedback.load());
    }

    void setSampleRate(int32_t sampleRate) {
        SignalModulator::setSampleRate(sampleRate);
        mCarrierFreqSmoother.setSmoothingTime(50.0f, sampleRate);
        mMixSmoother.setSmoothingTime(50.0f, sampleRate);
        mFeedbackSmoother.setSmoothingTime(50.0f, sampleRate);
    }

private:
    std::atomic<float> mCarrierFreq{440.0f};  // Hz
    std::atomic<float> mMix{0.5f};            // 0-1
    std::atomic<float> mFeedback{0.0f};       // 0-0.9

    float mCarrierPhase{0.0f};
    float mLastLeft{0.0f};
    float mLastRight{0.0f};

    ParameterSmoother mCarrierFreqSmoother;
    ParameterSmoother mMixSmoother;
    ParameterSmoother mFeedbackSmoother;
};

#endif // NOISYPAD_RINGMODULATOR_H
