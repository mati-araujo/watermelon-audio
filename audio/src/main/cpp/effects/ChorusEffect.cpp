#include "ChorusEffect.h"
#include <algorithm>
#include "../platform/Logger.h"

#define LOG_TAG "ChorusEffect"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ChorusEffect::ChorusEffect() {
    reset();

    // Initialize LFO phases with spread
    for (int v = 0; v < MAX_VOICES; ++v) {
        mLfoPhase[v] = static_cast<float>(v) / MAX_VOICES;
    }

    LOGI("ChorusEffect created");
}

void ChorusEffect::process(float* input, float* output, int numFrames) {
    const float sampleRate = static_cast<float>(mSampleRate);
    const float rate = mRate.load(std::memory_order_relaxed);
    const float depth = mDepth.load(std::memory_order_relaxed) / 100.0f;
    const float delayMs = mDelayMs.load(std::memory_order_relaxed);
    const float feedback = mFeedback.load(std::memory_order_relaxed) / 100.0f;
    const float mixTarget = mMix.load(std::memory_order_relaxed) / 100.0f;
    const int voices = mVoices.load(std::memory_order_relaxed);

    const float baseDelaySamples = delayMs * sampleRate / 1000.0f;
    const float modDepthSamples = baseDelaySamples * depth;
    const float lfoIncrement = rate / sampleRate;

    for (int i = 0; i < numFrames; ++i) {
        const int idx = i * 2;
        float inL = input[idx];
        float inR = input[idx + 1];

        // Write to delay line (with feedback)
        int prevIndex = (mWriteIndex - 1 + MAX_DELAY_SAMPLES) % MAX_DELAY_SAMPLES;
        mDelayLineL[mWriteIndex] = inL + mDelayLineL[prevIndex] * feedback;
        mDelayLineR[mWriteIndex] = inR + mDelayLineR[prevIndex] * feedback;

        // Sum modulated delay outputs from all voices
        float outL = 0.0f;
        float outR = 0.0f;

        for (int v = 0; v < voices; ++v) {
            // LFO with sine wave
            float lfoValue = std::sin(2.0f * static_cast<float>(M_PI) * mLfoPhase[v]);

            // Calculate modulated delay
            float modulatedDelay = baseDelaySamples + lfoValue * modDepthSamples;
            modulatedDelay = std::clamp(modulatedDelay, 1.0f, static_cast<float>(MAX_DELAY_SAMPLES - 1));

            // Read from delay lines with interpolation
            outL += interpolatedRead(mDelayLineL, modulatedDelay);
            outR += interpolatedRead(mDelayLineR, modulatedDelay);

            // Update LFO phase
            mLfoPhase[v] += lfoIncrement;
            if (mLfoPhase[v] >= 1.0f) mLfoPhase[v] -= 1.0f;
        }

        // Normalize by voice count
        outL /= static_cast<float>(voices);
        outR /= static_cast<float>(voices);

        // Mix dry/wet (smoothed per-sample)
        float mix = mMixSmoother.process(mixTarget);
        output[idx] = inL * (1.0f - mix) + outL * mix;
        output[idx + 1] = inR * (1.0f - mix) + outR * mix;

        // Advance write index
        mWriteIndex = (mWriteIndex + 1) % MAX_DELAY_SAMPLES;
    }
}

float ChorusEffect::interpolatedRead(const std::array<float, MAX_DELAY_SAMPLES>& line, float delaySamples) const {
    float readPos = static_cast<float>(mWriteIndex) - delaySamples;
    if (readPos < 0) readPos += MAX_DELAY_SAMPLES;

    int index0 = static_cast<int>(readPos);
    int index1 = (index0 + 1) % MAX_DELAY_SAMPLES;
    float frac = readPos - static_cast<float>(index0);

    // Linear interpolation
    return line[index0] * (1.0f - frac) + line[index1] * frac;
}

void ChorusEffect::reset() {
    mDelayLineL.fill(0.0f);
    mDelayLineR.fill(0.0f);
    mWriteIndex = 0;
}

void ChorusEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case RATE:
            mRate.store(std::clamp(value, 0.1f, 10.0f), std::memory_order_relaxed);
            break;
        case DEPTH:
            mDepth.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        case DELAY:
            mDelayMs.store(std::clamp(value, 1.0f, 30.0f), std::memory_order_relaxed);
            break;
        case FEEDBACK:
            mFeedback.store(std::clamp(value, -50.0f, 50.0f), std::memory_order_relaxed);
            break;
        case MIX:
            mMix.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        case VOICES:
            mVoices.store(std::clamp(static_cast<int>(value), 1, MAX_VOICES), std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

float ChorusEffect::getParam(int paramId) {
    switch (paramId) {
        case RATE: return mRate.load(std::memory_order_relaxed);
        case DEPTH: return mDepth.load(std::memory_order_relaxed);
        case DELAY: return mDelayMs.load(std::memory_order_relaxed);
        case FEEDBACK: return mFeedback.load(std::memory_order_relaxed);
        case MIX: return mMix.load(std::memory_order_relaxed);
        case VOICES: return static_cast<float>(mVoices.load(std::memory_order_relaxed));
        default: return 0.0f;
    }
}

void ChorusEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    mMixSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    reset();
    LOGI("Sample rate set to %d", sampleRate);
}
