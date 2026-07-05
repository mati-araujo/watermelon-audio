#include "PlateReverbEffect.h"
#include <algorithm>
#include <cmath>

PlateReverbEffect::PlateReverbEffect()
    : mPreDelayL(150.0f),
      mPreDelayR(150.0f),
      mLowCutL(48000.0f),
      mLowCutR(48000.0f),
      mHighCutL(48000.0f),
      mHighCutR(48000.0f) {
    mFdn.setDecayTime(2.4f);
    mFdn.setSize(0.55f);
    mFdn.setDamping(0.35f, 0.25f);
    mFdn.setModulation(0.12f);
    mLowCutL.setHighpass(120.0f, 0.707f);
    mLowCutR.setHighpass(120.0f, 0.707f);
    mHighCutL.setLowpass(9000.0f, 0.707f);
    mHighCutR.setLowpass(9000.0f, 0.707f);
    mMixSmooth.reset(0.28f);
    mPreDelaySmooth.reset(18.0f);
}

void PlateReverbEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);
    mPreDelayL.setSampleRate(sr);
    mPreDelayR.setSampleRate(sr);
    mPreDelayL.setMaxDelay(150.0f);
    mPreDelayR.setMaxDelay(150.0f);
    mFdn.setSampleRate(sampleRate);
    mLowCutL.setSampleRate(sr);
    mLowCutR.setSampleRate(sr);
    mHighCutL.setSampleRate(sr);
    mHighCutR.setSampleRate(sr);
    mMixSmooth.setSmoothingTime(10.0f, sr);
    mPreDelaySmooth.setSmoothingTime(20.0f, sr);
}

void PlateReverbEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_DECAY:
            mDecay.store(std::clamp(value, 0.5f, 8.0f), std::memory_order_relaxed);
            mFdn.setDecayTime(std::clamp(value, 0.5f, 8.0f));
            break;
        case PARAM_PRE_DELAY:
            mPreDelay.store(std::clamp(value, 0.0f, 150.0f), std::memory_order_relaxed);
            break;
        case PARAM_DAMPING:
            mDamping.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            mFdn.setDamping(std::clamp(value, 0.0f, 1.0f), 0.25f);
            break;
        case PARAM_MODULATION:
            mModulation.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            mFdn.setModulation(std::clamp(value, 0.0f, 1.0f));
            break;
        case PARAM_LOW_CUT:
            mLowCut.store(std::clamp(value, 20.0f, 500.0f), std::memory_order_relaxed);
            break;
        case PARAM_HIGH_CUT:
            mHighCut.store(std::clamp(value, 1000.0f, 20000.0f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

float PlateReverbEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_DECAY: return mDecay.load(std::memory_order_relaxed);
        case PARAM_PRE_DELAY: return mPreDelay.load(std::memory_order_relaxed);
        case PARAM_DAMPING: return mDamping.load(std::memory_order_relaxed);
        case PARAM_MODULATION: return mModulation.load(std::memory_order_relaxed);
        case PARAM_LOW_CUT: return mLowCut.load(std::memory_order_relaxed);
        case PARAM_HIGH_CUT: return mHighCut.load(std::memory_order_relaxed);
        case PARAM_MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void PlateReverbEffect::process(float* input, float* output, int numFrames) {
    float lowCut = mLowCut.load(std::memory_order_relaxed);
    float highCut = mHighCut.load(std::memory_order_relaxed);
    if (std::abs(lowCut - mLastLowCut) > 1.0f) {
        mLowCutL.setHighpass(lowCut, 0.707f);
        mLowCutR.setHighpass(lowCut, 0.707f);
        mLastLowCut = lowCut;
    }
    if (std::abs(highCut - mLastHighCut) > 5.0f) {
        mHighCutL.setLowpass(highCut, 0.707f);
        mHighCutR.setLowpass(highCut, 0.707f);
        mLastHighCut = highCut;
    }

    float preDelay = mPreDelay.load(std::memory_order_relaxed);
    float mixTarget = mMix.load(std::memory_order_relaxed);

    for (int i = 0; i < numFrames; ++i) {
        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];
        float mix = mMixSmooth.process(mixTarget);
        float preMs = mPreDelaySmooth.process(preDelay);

        mPreDelayL.write(dryL);
        mPreDelayR.write(dryR);
        float preSamples = std::max(preMs * static_cast<float>(mSampleRate) / 1000.0f, 1.0f);
        float preL = mPreDelayL.readInterpolated(preSamples);
        float preR = mPreDelayR.readInterpolated(preSamples);

        float wetL = 0.0f;
        float wetR = 0.0f;
        mFdn.process(preL, preR, wetL, wetR);
        wetL = mHighCutL.process(mLowCutL.process(wetL));
        wetR = mHighCutR.process(mLowCutR.process(wetR));

        if (!std::isfinite(wetL)) wetL = 0.0f;
        if (!std::isfinite(wetR)) wetR = 0.0f;

        output[i * 2] = dryL + (wetL - dryL) * mix;
        output[i * 2 + 1] = dryR + (wetR - dryR) * mix;
    }
}

void PlateReverbEffect::reset() {
    mPreDelayL.clear();
    mPreDelayR.clear();
    mFdn.reset();
    mLowCutL.reset();
    mLowCutR.reset();
    mHighCutL.reset();
    mHighCutR.reset();
}
