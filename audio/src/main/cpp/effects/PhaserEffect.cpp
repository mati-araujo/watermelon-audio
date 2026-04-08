#include "PhaserEffect.h"
#include <algorithm>
#include "../platform/Logger.h"

#define LOG_TAG "PhaserEffect"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PhaserEffect::PhaserEffect() {
    reset();
    LOGI("PhaserEffect created");
}

void PhaserEffect::process(float* input, float* output, int numFrames) {
    const float sampleRate = static_cast<float>(mSampleRate);
    const float rate = mRate.load(std::memory_order_relaxed);
    const float depth = mDepth.load(std::memory_order_relaxed) / 100.0f;
    const int stages = mStages.load(std::memory_order_relaxed);
    const float feedbackTarget = mFeedback.load(std::memory_order_relaxed) / 100.0f;
    const float mixTarget = mMix.load(std::memory_order_relaxed) / 100.0f;

    const float lfoIncrement = rate / sampleRate;

    for (int i = 0; i < numFrames; ++i) {
        const int idx = i * 2;

        // LFO (triangle wave for smoother sweep)
        float lfoValue = 2.0f * std::abs(2.0f * mLfoPhase - 1.0f) - 1.0f;  // -1 to +1

        // Calculate center frequency from LFO
        float freqRange = (MAX_FREQ - MIN_FREQ) * depth;
        float centerFreq = MIN_FREQ + (MAX_FREQ - MIN_FREQ) / 2.0f;
        float modFreq = centerFreq + lfoValue * freqRange / 2.0f;

        // Calculate all-pass coefficient from frequency
        // Using bilinear transform approximation
        float wc = static_cast<float>(M_PI) * modFreq / sampleRate;
        float tanWc = std::tan(wc);
        float coefficient = (tanWc - 1.0f) / (tanWc + 1.0f);

        // Smooth feedback and mix per-sample
        float feedback = mFeedbackSmoother.process(feedbackTarget);
        float mix = mMixSmoother.process(mixTarget);

        // Get input with feedback
        float inL = input[idx] + mFeedbackL * feedback;
        float inR = input[idx + 1] + mFeedbackR * feedback;

        // Process through all-pass chain
        float outL = inL;
        float outR = inR;

        for (int s = 0; s < stages; ++s) {
            outL = processAllPass(outL, mPrevInputL[s], mAllPassStateL[s], coefficient);
            outR = processAllPass(outR, mPrevInputR[s], mAllPassStateR[s], coefficient);
        }

        // Store for feedback (limit to prevent runaway)
        mFeedbackL = std::clamp(outL, -1.0f, 1.0f);
        mFeedbackR = std::clamp(outR, -1.0f, 1.0f);

        // Mix dry/wet
        output[idx] = input[idx] * (1.0f - mix) + outL * mix;
        output[idx + 1] = input[idx + 1] * (1.0f - mix) + outR * mix;

        // Update LFO
        mLfoPhase += lfoIncrement;
        if (mLfoPhase >= 1.0f) mLfoPhase -= 1.0f;
    }
}

float PhaserEffect::processAllPass(float input, float& prevInput, float& state, float coefficient) {
    // First-order all-pass filter:
    // y[n] = coef * x[n] + x[n-1] - coef * y[n-1]
    float output = coefficient * input + prevInput - coefficient * state;
    prevInput = input;
    state = output;
    return output;
}

void PhaserEffect::reset() {
    mAllPassStateL.fill(0.0f);
    mAllPassStateR.fill(0.0f);
    mPrevInputL.fill(0.0f);
    mPrevInputR.fill(0.0f);
    mLfoPhase = 0.0f;
    mFeedbackL = 0.0f;
    mFeedbackR = 0.0f;
}

void PhaserEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case RATE:
            mRate.store(std::clamp(value, 0.01f, 10.0f), std::memory_order_relaxed);
            break;
        case DEPTH:
            mDepth.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        case STAGES: {
            // Only allow valid stage counts
            int stages = static_cast<int>(value);
            if (stages <= 2) stages = 2;
            else if (stages <= 4) stages = 4;
            else if (stages <= 6) stages = 6;
            else if (stages <= 8) stages = 8;
            else stages = 12;
            mStages.store(stages, std::memory_order_relaxed);
            break;
        }
        case FEEDBACK:
            mFeedback.store(std::clamp(value, -90.0f, 90.0f), std::memory_order_relaxed);
            break;
        case MIX:
            mMix.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

float PhaserEffect::getParam(int paramId) {
    switch (paramId) {
        case RATE: return mRate.load(std::memory_order_relaxed);
        case DEPTH: return mDepth.load(std::memory_order_relaxed);
        case STAGES: return static_cast<float>(mStages.load(std::memory_order_relaxed));
        case FEEDBACK: return mFeedback.load(std::memory_order_relaxed);
        case MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void PhaserEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    mMixSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    mFeedbackSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    reset();
    LOGI("Sample rate set to %d", sampleRate);
}
