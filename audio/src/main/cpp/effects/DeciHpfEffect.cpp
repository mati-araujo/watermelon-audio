#include "DeciHpfEffect.h"
#include <cmath>

DeciHpfEffect::DeciHpfEffect() {
    mBitDepthSmooth.reset(12.0f);
    mCutoffSmooth.reset(300.0f);
    mSRSmooth.reset(12000.0f);
    mMixSmooth.reset(1.0f);
}


int DeciHpfEffect::getLatencySamples() const {
    // Mismo calculo que hace process(): step = sampleRate / targetSR, y el hold
    // no entrega su primer valor nuevo hasta que el contador lo alcanza.
    const float targetSR = mTargetSR.load(std::memory_order_relaxed);
    const float step = static_cast<float>(mSampleRate) / std::max(targetSR, 100.0f);
    if (!(step > 1.0f)) return 0;  // sin diezmado no hay hold que esperar
    return static_cast<int>(std::ceil(step)) - 1;
}

void DeciHpfEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);
    mBitDepthSmooth.setSmoothingTime(10.0f, sr);
    mCutoffSmooth.setSmoothingTime(10.0f, sr);
    mSRSmooth.setSmoothingTime(10.0f, sr);
    mMixSmooth.setSmoothingTime(5.0f, sr);

    // Initialize HPF
    mHpfL.setHighpass(300.0f, 0.707f);
    mHpfR.setHighpass(300.0f, 0.707f);
    mLastCutoff = 300.0f;
}

void DeciHpfEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_BIT_DEPTH:
            mBitDepth.store(std::clamp(value, 1.0f, 24.0f), std::memory_order_relaxed);
            break;
        case PARAM_HPF_CUTOFF:
            mHpfCutoff.store(std::clamp(value, 20.0f, 8000.0f), std::memory_order_relaxed);
            break;
        case PARAM_SAMPLE_RATE:
            mTargetSR.store(std::clamp(value, 100.0f, 48000.0f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
    }
}

float DeciHpfEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_BIT_DEPTH:   return mBitDepth.load(std::memory_order_relaxed);
        case PARAM_HPF_CUTOFF:  return mHpfCutoff.load(std::memory_order_relaxed);
        case PARAM_SAMPLE_RATE: return mTargetSR.load(std::memory_order_relaxed);
        case PARAM_MIX:         return mMix.load(std::memory_order_relaxed);
        default:                return 0.0f;
    }
}

void DeciHpfEffect::process(float* input, float* output, int numFrames) {
    float bitDepth = mBitDepth.load(std::memory_order_relaxed);
    float cutoff = mHpfCutoff.load(std::memory_order_relaxed);
    float targetSR = mTargetSR.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);

    // Recalculate HPF only when cutoff changes significantly (>0.5 Hz)
    float smoothCutoff = mCutoffSmooth.process(cutoff);
    if (std::abs(smoothCutoff - mLastCutoff) > 0.5f) {
        mHpfL.setHighpass(smoothCutoff, 0.707f);
        mHpfR.setHighpass(smoothCutoff, 0.707f);
        mLastCutoff = smoothCutoff;
    }

    for (int i = 0; i < numFrames; ++i) {
        float smoothBits = mBitDepthSmooth.process(bitDepth);
        float smoothSR = mSRSmooth.process(targetSR);
        float smoothMix = mMixSmooth.process(mix);

        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        // Stage 1: HPF
        float hpfL = mHpfL.process(dryL);
        float hpfR = mHpfR.process(dryR);

        // Stage 2: Sample rate reduction (zero-order hold)
        float step = static_cast<float>(mSampleRate) / std::max(smoothSR, 100.0f);
        mHoldCounter += 1.0f;
        if (mHoldCounter >= step) {
            mHoldCounter -= step;
            mHoldL = hpfL;
            mHoldR = hpfR;
        }

        // Stage 3: Bit depth reduction
        float levels = std::pow(2.0f, smoothBits);
        float wetL = std::floor(mHoldL * levels) / levels;
        float wetR = std::floor(mHoldR * levels) / levels;

        // Mix
        output[i * 2]     = dryL + (wetL - dryL) * smoothMix;
        output[i * 2 + 1] = dryR + (wetR - dryR) * smoothMix;
    }
}
