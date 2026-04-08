#ifndef NOISYPAD_PWMMODULATOR_H
#define NOISYPAD_PWMMODULATOR_H

#include "SignalModulator.h"
#include "../dsp/LFO.h"
#include "../dsp/ParameterSmoother.h"
#include <cmath>

/**
 * PWM (Pulse Width Modulation) Modulator.
 *
 * Creates a vintage synthesizer effect by modulating the duty cycle
 * of the audio signal, producing a rich, evolving harmonic content.
 *
 * Works by comparing the input signal with a threshold that varies over time.
 *
 * Parameters:
 * - Pulse Width (0.1 - 0.9): Base duty cycle
 * - Rate (0.1 - 20 Hz): Modulation speed
 * - Depth (0.0 - 1.0): Modulation amount
 */
class PWMModulator : public SignalModulator {
public:
    enum ParamId {
        PULSE_WIDTH = 0,  // 0.1 - 0.9
        RATE = 1,         // Hz: 0.1 - 20
        DEPTH = 2         // 0.0 - 1.0
    };

    PWMModulator() {
        mLFO.setWaveform(LFO::Waveform::TRIANGLE); // Triangle LFO for smooth PWM
        mLFO.setRate(2.0f);

        mPulseWidthSmoother.setSmoothingTime(50.0f, mSampleRate);
        mRateSmoother.setSmoothingTime(100.0f, mSampleRate);
        mDepthSmoother.setSmoothingTime(50.0f, mSampleRate);

        reset();
    }

    void process(float* buffer, int32_t numFrames) override {
        for (int32_t i = 0; i < numFrames; ++i) {
            // Smooth parameters
            const float pulseWidth = mPulseWidthSmoother.process(mPulseWidth.load());
            const float rate = mRateSmoother.process(mRate.load());
            const float depth = mDepthSmoother.process(mDepth.load());

            // Update LFO rate
            mLFO.setRate(rate);

            // Get LFO value (-1 to 1)
            const float lfoValue = mLFO.process();

            // Calculate modulated pulse width
            // Center around base pulse width, modulate by depth
            float modulatedWidth = pulseWidth + (lfoValue * depth * 0.4f);
            modulatedWidth = std::max(0.1f, std::min(0.9f, modulatedWidth));

            // PWM effect: compare signal with threshold
            const int32_t idx = i * 2;
            const float leftSample = buffer[idx];
            const float rightSample = buffer[idx + 1];

            // Calculate threshold based on modulated width
            // Width of 0.5 = no change, <0.5 = negative threshold, >0.5 = positive threshold
            const float threshold = (modulatedWidth - 0.5f) * 2.0f;

            // Apply PWM: if sample > threshold, keep it, else invert
            // This creates the characteristic PWM sound
            const float leftPWM = (leftSample > threshold) ? leftSample : -leftSample * 0.5f;
            const float rightPWM = (rightSample > threshold) ? rightSample : -rightSample * 0.5f;

            // Mix with original signal for musicality
            const float mix = 0.7f; // 70% PWM, 30% original
            buffer[idx] = leftPWM * mix + leftSample * (1.0f - mix);
            buffer[idx + 1] = rightPWM * mix + rightSample * (1.0f - mix);
        }
    }

    void setParameter(int paramId, float value) override {
        switch (paramId) {
            case PULSE_WIDTH:
                // Map 0-1 to 0.1-0.9
                mPulseWidth.store(0.1f + value * 0.8f);
                break;
            case RATE:
                // Map 0-1 to 0.1-20 Hz (logarithmic)
                mRate.store(0.1f + value * value * 19.9f);
                break;
            case DEPTH:
                // Direct mapping 0-1
                mDepth.store(std::max(0.0f, std::min(1.0f, value)));
                break;
        }
    }

    void reset() override {
        mLFO.reset();
        mPulseWidthSmoother.reset(mPulseWidth.load());
        mRateSmoother.reset(mRate.load());
        mDepthSmoother.reset(mDepth.load());
    }

    void setSampleRate(int32_t sampleRate) {
        SignalModulator::setSampleRate(sampleRate);
        mLFO.setSampleRate(static_cast<float>(sampleRate));
        mPulseWidthSmoother.setSmoothingTime(50.0f, sampleRate);
        mRateSmoother.setSmoothingTime(100.0f, sampleRate);
        mDepthSmoother.setSmoothingTime(50.0f, sampleRate);
    }

private:
    std::atomic<float> mPulseWidth{0.5f};  // 0.1-0.9
    std::atomic<float> mRate{2.0f};        // Hz
    std::atomic<float> mDepth{0.4f};       // 0-1

    LFO mLFO;

    ParameterSmoother mPulseWidthSmoother;
    ParameterSmoother mRateSmoother;
    ParameterSmoother mDepthSmoother;
};

#endif // NOISYPAD_PWMMODULATOR_H
