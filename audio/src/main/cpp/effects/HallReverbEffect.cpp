#include "HallReverbEffect.h"
#include <cmath>
#include <algorithm>

void HallReverbEffect::reset() {
    // Clear all delay line and FDN state. RT-safe: DelayLine::clear()
    // and FDN::reset() zero-fill existing buffers without resizing.
    mPreDelayL.clear();
    mPreDelayR.clear();
    mEarlyL.clear();
    mEarlyR.clear();
    mFdn.reset();

    // WD-3.2 — los CINCO ParameterSmoother, que este reset() no tocaba. Es el
    // que mas se olvidaba de los seis del grupo B.
    mDecaySmooth.reset(mDecayTime.load(std::memory_order_relaxed));
    mSizeSmooth.reset(mSize.load(std::memory_order_relaxed));
    mPreDelaySmooth.reset(mPreDelay.load(std::memory_order_relaxed));
    mDiffusionSmooth.reset(mDiffusion.load(std::memory_order_relaxed));
    mMixSmooth.reset(mMix.load(std::memory_order_relaxed));
}

HallReverbEffect::HallReverbEffect()
    : mPreDelayL(200.0f),
      mPreDelayR(200.0f),
      mEarlyL(100.0f),
      mEarlyR(100.0f) {
    // Initialize smoothers
    mDecaySmooth.reset(3.0f);
    mSizeSmooth.reset(0.7f);
    mPreDelaySmooth.reset(30.0f);
    mDiffusionSmooth.reset(0.8f);
    mMixSmooth.reset(0.3f);
}

void HallReverbEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);

    mPreDelayL = DelayLine(200.0f, sr);
    mPreDelayR = DelayLine(200.0f, sr);
    mEarlyL = DelayLine(100.0f, sr);
    mEarlyR = DelayLine(100.0f, sr);

    mFdn.setSampleRate(sampleRate);

    mDecaySmooth.setSmoothingTime(30.0f, sampleRate);
    mSizeSmooth.setSmoothingTime(30.0f, sampleRate);
    mPreDelaySmooth.setSmoothingTime(30.0f, sampleRate);
    mDiffusionSmooth.setSmoothingTime(20.0f, sampleRate);
    mMixSmooth.setSmoothingTime(10.0f, sampleRate);

    // Apply current damping and FDN settings
    float decay = mDecayTime.load(std::memory_order_relaxed);
    float size = mSize.load(std::memory_order_relaxed);
    float hfDamp = mHfDamping.load(std::memory_order_relaxed);
    float lfDamp = mLfDamping.load(std::memory_order_relaxed);
    float mod = mModulation.load(std::memory_order_relaxed);

    mFdn.setDecayTime(decay);
    mFdn.setSize(size);
    mFdn.setDamping(hfDamp, lfDamp);
    mFdn.setModulation(mod);
}

void HallReverbEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_DECAY_TIME:
            mDecayTime.store(std::clamp(value, 0.5f, 15.0f), std::memory_order_relaxed);
            mFdn.setDecayTime(std::clamp(value, 0.5f, 15.0f));
            break;
        case PARAM_SIZE:
            mSize.store(std::clamp(value, 0.1f, 1.0f), std::memory_order_relaxed);
            mFdn.setSize(std::clamp(value, 0.1f, 1.0f));
            break;
        case PARAM_PRE_DELAY:
            mPreDelay.store(std::clamp(value, 0.0f, 150.0f), std::memory_order_relaxed);
            break;
        case PARAM_DIFFUSION:
            mDiffusion.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_HF_DAMPING:
            mHfDamping.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            mFdn.setDamping(std::clamp(value, 0.0f, 1.0f),
                           mLfDamping.load(std::memory_order_relaxed));
            break;
        case PARAM_LF_DAMPING:
            mLfDamping.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            mFdn.setDamping(mHfDamping.load(std::memory_order_relaxed),
                           std::clamp(value, 0.0f, 1.0f));
            break;
        case PARAM_MODULATION:
            mModulation.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            mFdn.setModulation(std::clamp(value, 0.0f, 1.0f));
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
    }
}

float HallReverbEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_DECAY_TIME: return mDecayTime.load(std::memory_order_relaxed);
        case PARAM_SIZE: return mSize.load(std::memory_order_relaxed);
        case PARAM_PRE_DELAY: return mPreDelay.load(std::memory_order_relaxed);
        case PARAM_DIFFUSION: return mDiffusion.load(std::memory_order_relaxed);
        case PARAM_HF_DAMPING: return mHfDamping.load(std::memory_order_relaxed);
        case PARAM_LF_DAMPING: return mLfDamping.load(std::memory_order_relaxed);
        case PARAM_MODULATION: return mModulation.load(std::memory_order_relaxed);
        case PARAM_MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void HallReverbEffect::process(float* input, float* output, int numFrames) {
    // Load params once (decay is applied via the feedback coefficients on
    // param change, not per-block).
    float size = mSize.load(std::memory_order_relaxed);
    float preDelayMs = mPreDelay.load(std::memory_order_relaxed);
    float diffusion = mDiffusion.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);

    float sr = static_cast<float>(mSampleRate);

    for (int i = 0; i < numFrames; ++i) {
        float smoothSize = mSizeSmooth.process(size);
        float smoothPreDelay = mPreDelaySmooth.process(preDelayMs);
        float smoothDiffusion = mDiffusionSmooth.process(diffusion);
        float smoothMix = mMixSmooth.process(mix);

        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        // Pre-delay
        mPreDelayL.write(dryL);
        mPreDelayR.write(dryR);
        float preDelSamples = smoothPreDelay * sr / 1000.0f;
        preDelSamples = std::max(preDelSamples, 1.0f);
        float preL = mPreDelayL.readInterpolated(preDelSamples);
        float preR = mPreDelayR.readInterpolated(preDelSamples);

        // Early reflections (multitap)
        mEarlyL.write(preL);
        mEarlyR.write(preR);

        float earlyL = 0.0f, earlyR = 0.0f;
        for (int t = 0; t < NUM_EARLY_TAPS; ++t) {
            float tapMs = EARLY_DELAYS_MS[t] * smoothSize;
            float tapSamples = tapMs * sr / 1000.0f;
            tapSamples = std::max(tapSamples, 1.0f);
            earlyL += mEarlyL.readInterpolated(tapSamples) * EARLY_GAINS[t];
            earlyR += mEarlyR.readInterpolated(tapSamples) * EARLY_GAINS[t];
        }

        // Late reverb via FDN
        float lateL = 0.0f, lateR = 0.0f;
        mFdn.process(preL, preR, lateL, lateR);

        // Mix early + late (diffusion controls balance, smoothed)
        float wetL = earlyL * (1.0f - smoothDiffusion) + lateL * smoothDiffusion;
        float wetR = earlyR * (1.0f - smoothDiffusion) + lateR * smoothDiffusion;

        // Denormal protection
        if (std::abs(wetL) < 1e-20f) wetL = 0.0f;
        if (std::abs(wetR) < 1e-20f) wetR = 0.0f;

        // Dry/Wet mix
        float outL = dryL + (wetL - dryL) * smoothMix;
        float outR = dryR + (wetR - dryR) * smoothMix;

        // NaN/Inf protection
        if (!std::isfinite(outL)) outL = dryL;
        if (!std::isfinite(outR)) outR = dryR;

        output[i * 2]     = outL;
        output[i * 2 + 1] = outR;
    }
}
