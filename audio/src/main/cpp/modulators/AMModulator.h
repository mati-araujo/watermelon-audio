#ifndef NOISYPAD_AMMODULATOR_H
#define NOISYPAD_AMMODULATOR_H

#include "SignalModulator.h"
#include "../dsp/LFO.h"
#include "../dsp/ParameterSmoother.h"
#include <cmath>

/**
 * AM (Amplitude Modulation) Modulator.
 *
 * Modulates the amplitude of the audio signal using an LFO,
 * creating tremolo and vibrato effects.
 *
 * Parameters:
 * - Rate (0.01 - 20 Hz): LFO frequency
 * - Depth (0.0 - 1.0): Modulation amount
 * - Waveform (0-4): LFO shape (Sine, Triangle, Square, Sawtooth, Random)
 */
class AMModulator : public SignalModulator {
public:
    enum ParamId {
        RATE = 0,      // Hz: 0.01 - 20
        DEPTH = 1,     // 0.0 - 1.0
        WAVEFORM = 2   // 0-4: LFO waveform type
    };

    AMModulator() {
        mLFO.setWaveform(LFO::Waveform::SINE);
        mLFO.setRate(5.0f);

        mRateSmoother.setSmoothingTime(100.0f, mSampleRate);
        mDepthSmoother.setSmoothingTime(50.0f, mSampleRate);

        reset();
    }

    void process(float* buffer, int32_t numFrames) override {
        for (int32_t i = 0; i < numFrames; ++i) {
            // Smooth parameters
            const float rate = mRateSmoother.process(mRate.load());
            const float depth = mDepthSmoother.process(mDepth.load());

            // Update LFO rate
            mLFO.setRate(rate);

            // Get LFO value (0-1)
            const float lfoValue = mLFO.processUnipolar();

            // Calculate modulation
            // When depth = 0, no modulation (gain = 1.0)
            // When depth = 1, full modulation (gain varies 0-1)
            const float gain = 1.0f - depth + (depth * lfoValue);

            // Apply to stereo buffer
            const int32_t idx = i * 2;
            buffer[idx] *= gain;     // Left
            buffer[idx + 1] *= gain; // Right
        }
    }

    void setParameter(int paramId, float value) override {
        switch (paramId) {
            case RATE:
                // Map 0-1 to 0.01-20 Hz (logarithmic)
                mRate.store(0.01f + value * value * 19.99f);
                break;
            case DEPTH:
                // Direct mapping 0-1
                mDepth.store(std::max(0.0f, std::min(1.0f, value)));
                break;
            case WAVEFORM: {
                // Map 0-1 to discrete waveform types
                int waveformIndex = static_cast<int>(value * 4.99f);
                waveformIndex = std::max(0, std::min(4, waveformIndex));

                LFO::Waveform waveform;
                switch (waveformIndex) {
                    case 0: waveform = LFO::Waveform::SINE; break;
                    case 1: waveform = LFO::Waveform::TRIANGLE; break;
                    case 2: waveform = LFO::Waveform::SQUARE; break;
                    case 3: waveform = LFO::Waveform::SAWTOOTH; break;
                    case 4: waveform = LFO::Waveform::RANDOM; break;
                    default: waveform = LFO::Waveform::SINE; break;
                }

                mLFO.setWaveform(waveform);
                break;
            }
        }
    }

    void reset() override {
        mLFO.reset();
        mRateSmoother.reset(mRate.load());
        mDepthSmoother.reset(mDepth.load());
    }

    void setSampleRate(int32_t sampleRate) {
        SignalModulator::setSampleRate(sampleRate);
        mLFO.setSampleRate(static_cast<float>(sampleRate));
        mRateSmoother.setSmoothingTime(100.0f, sampleRate);
        mDepthSmoother.setSmoothingTime(50.0f, sampleRate);
    }

private:
    std::atomic<float> mRate{5.0f};    // Hz
    std::atomic<float> mDepth{0.5f};   // 0-1

    LFO mLFO;

    ParameterSmoother mRateSmoother;
    ParameterSmoother mDepthSmoother;
};

#endif // NOISYPAD_AMMODULATOR_H
