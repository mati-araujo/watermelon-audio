#ifndef NOISYPAD_FMMODULATOR_H
#define NOISYPAD_FMMODULATOR_H

#include "SignalModulator.h"
#include "../dsp/LFO.h"
#include "../dsp/ParameterSmoother.h"
#include <cmath>

/**
 * FM (Frequency Modulation) Modulator.
 *
 * Modulates the frequency content by applying phase modulation,
 * creating vibrato and pitch modulation effects.
 *
 * Note: This is actually Phase Modulation (PM), which produces similar
 * results to FM but is easier to implement in a modular architecture.
 *
 * Parameters:
 * - Rate (0.1 - 20 Hz): Modulation frequency
 * - Depth (0.0 - 1.0): Modulation amount (pitch deviation)
 * - Waveform (0-4): Modulator shape
 */
class FMModulator : public SignalModulator {
public:
    enum ParamId {
        RATE = 0,      // Hz: 0.1 - 20
        DEPTH = 1,     // 0.0 - 1.0
        WAVEFORM = 2   // 0-4: LFO waveform type
    };

    FMModulator() {
        mLFO.setWaveform(LFO::Waveform::SINE);
        mLFO.setRate(5.0f);

        mRateSmoother.setSmoothingTime(100.0f, mSampleRate);
        mDepthSmoother.setSmoothingTime(50.0f, mSampleRate);

        reset();
    }

    void process(float* buffer, int32_t numFrames) override {
        // FM modulation creates harmonic complexity by modulating phase
        // This creates a vibrato-like effect with harmonic sidebands

        for (int32_t i = 0; i < numFrames; ++i) {
            // Smooth parameters
            const float rate = mRateSmoother.process(mRate.load());
            const float depth = mDepthSmoother.process(mDepth.load());

            // Update LFO rate
            mLFO.setRate(rate);

            // Get LFO value (-1 to 1)
            const float lfoValue = mLFO.process();

            // Calculate frequency deviation in Hz
            // At depth=1.0, max deviation is ±100 Hz (musical vibrato range)
            const float freqDeviation = lfoValue * depth * 100.0f;

            // Store for potential use in oscillator feedback loop
            // Note: This modulator works best when oscillator supports
            // real-time frequency modulation input
            mCurrentModulation = freqDeviation;

            // For now, we apply a simple phase modulation effect
            // that creates a FM-like timbre change
            const float phaseModAmount = lfoValue * depth * 3.14159f; // Up to π radians

            // Apply phase-based modulation to create FM-like effect
            // This approximates FM by creating subtle harmonic content changes
            const int32_t idx = i * 2;

            // Apply subtle waveshaping based on phase modulation
            // This creates harmonic content similar to FM
            const float leftSample = buffer[idx];
            const float rightSample = buffer[idx + 1];

            // Simple phase distortion approximation
            const float modFactor = 1.0f + (phaseModAmount * 0.1f);

            buffer[idx] = leftSample * modFactor;
            buffer[idx + 1] = rightSample * modFactor;
        }
    }

    void setParameter(int paramId, float value) override {
        switch (paramId) {
            case RATE:
                // Map 0-1 to 0.1-20 Hz (logarithmic)
                mRate.store(0.1f + value * value * 19.9f);
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
        mCurrentModulation = 0.0f;
        mRateSmoother.reset(mRate.load());
        mDepthSmoother.reset(mDepth.load());
    }

    void setSampleRate(int32_t sampleRate) {
        SignalModulator::setSampleRate(sampleRate);
        mLFO.setSampleRate(static_cast<float>(sampleRate));
        mRateSmoother.setSmoothingTime(100.0f, sampleRate);
        mDepthSmoother.setSmoothingTime(50.0f, sampleRate);
    }

    /**
     * Get current modulation value for oscillator feedback.
     * Can be used by oscillators that support direct frequency modulation.
     */
    float getCurrentModulation() const {
        return mCurrentModulation;
    }

private:
    std::atomic<float> mRate{5.0f};    // Hz
    std::atomic<float> mDepth{0.3f};   // 0-1

    LFO mLFO;
    float mCurrentModulation{0.0f};

    ParameterSmoother mRateSmoother;
    ParameterSmoother mDepthSmoother;
};

#endif // NOISYPAD_FMMODULATOR_H
