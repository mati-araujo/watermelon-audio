#include "BeatGrainEffect.h"
#include <cmath>
#include <algorithm>
#include "../platform/Logger.h"

#define LOG_TAG "BeatGrainEffect"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)

BeatGrainEffect::BeatGrainEffect() {
    mMixSmooth.reset(0.5f);
    LOGI("BeatGrainEffect created");
}

void BeatGrainEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    mMixSmooth.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    mGrainEngine.setSampleRate(sampleRate);
    mSchedulerCounter = 0.0f;
    LOGI("Sample rate set to %d", sampleRate);
}

void BeatGrainEffect::setBpm(float bpm) {
    mBpm.store(std::clamp(bpm, 20.0f, 300.0f), std::memory_order_relaxed);
}

float BeatGrainEffect::getGrainIntervalSamples(int density, float bpm) {
    // density: 0=1/4 note, 1=1/8, 2=1/16, 3=1/32
    float beatsPerSecond = bpm / 60.0f;
    float divisor = std::pow(2.0f, static_cast<float>(density));
    float grainsPerSecond = beatsPerSecond * divisor;
    if (grainsPerSecond < 0.01f) grainsPerSecond = 0.01f;
    return static_cast<float>(mSampleRate) / grainsPerSecond;
}

void BeatGrainEffect::process(float* input, float* output, int numFrames) {
    // Load all params once
    float grainSize = mGrainSize.load(std::memory_order_relaxed);
    int density = mDensity.load(std::memory_order_relaxed);
    float spread = mPositionSpread.load(std::memory_order_relaxed);
    float pitch = mPitchShift.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);
    float bpm = mBpm.load(std::memory_order_relaxed);

    float interval = getGrainIntervalSamples(density, bpm);

    for (int i = 0; i < numFrames; ++i) {
        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        // Write to circular buffer
        mGrainEngine.writeToBuffer(dryL, dryR);

        // Scheduler: trigger grain when counter expires
        mSchedulerCounter -= 1.0f;
        if (mSchedulerCounter <= 0.0f) {
            mSchedulerCounter += interval;
            mGrainEngine.triggerGrain(grainSize, spread, pitch);
        }

        // Process active grain voices
        float grainL, grainR;
        mGrainEngine.process(grainL, grainR);

        // Mix dry/wet (smoothed)
        float smoothMix = mMixSmooth.process(mix);
        float outL = dryL + (grainL - dryL) * smoothMix;
        float outR = dryR + (grainR - dryR) * smoothMix;

        // Output protection
        if (!std::isfinite(outL)) outL = dryL;
        if (!std::isfinite(outR)) outR = dryR;

        output[i * 2]     = outL;
        output[i * 2 + 1] = outR;
    }
}

void BeatGrainEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_GRAIN_SIZE:
            mGrainSize.store(std::clamp(value, 1.0f, 200.0f), std::memory_order_relaxed);
            break;
        case PARAM_DENSITY:
            mDensity.store(std::clamp(static_cast<int>(value), 0, 3), std::memory_order_relaxed);
            break;
        case PARAM_POSITION_SPREAD:
            mPositionSpread.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_PITCH_SHIFT:
            mPitchShift.store(std::clamp(value, -12.0f, 12.0f), std::memory_order_relaxed);
            break;
        case PARAM_BUFFER_LENGTH:
            mBufferLength.store(std::clamp(value, 0.5f, 4.0f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

float BeatGrainEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_GRAIN_SIZE: return mGrainSize.load(std::memory_order_relaxed);
        case PARAM_DENSITY: return static_cast<float>(mDensity.load(std::memory_order_relaxed));
        case PARAM_POSITION_SPREAD: return mPositionSpread.load(std::memory_order_relaxed);
        case PARAM_PITCH_SHIFT: return mPitchShift.load(std::memory_order_relaxed);
        case PARAM_BUFFER_LENGTH: return mBufferLength.load(std::memory_order_relaxed);
        case PARAM_MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}
