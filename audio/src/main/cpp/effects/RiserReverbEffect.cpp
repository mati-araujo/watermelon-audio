#include "RiserReverbEffect.h"
#include <cmath>
#include <algorithm>

void RiserReverbEffect::reset() {
    // Clear the multitap delay buffers (main tail) and the pre-delay
    // buffers. Also reset the diffusion/darkening filters to zero
    // state so stale tap content doesn't re-emerge through them.
    mPreDelayL.clear();
    mPreDelayR.clear();
    mTapDelayL.clear();
    mTapDelayR.clear();
    mDiffuseL1.reset();
    mDiffuseL2.reset();
    mDiffuseR1.reset();
    mDiffuseR2.reset();
    mTapLpfL.reset();
    mTapLpfR.reset();
}

RiserReverbEffect::RiserReverbEffect()
    : mPreDelayL(200.0f),
      mPreDelayR(200.0f),
      mTapDelayL(1200.0f),
      mTapDelayR(1200.0f) {
    // Initialize smoothers
    mAttackSmooth.reset(800.0f);
    mSizeSmooth.reset(0.6f);
    mMixSmooth.reset(0.5f);
    mDampSmooth.reset(0.4f);

    // Initialize diffusion filters
    setupAllpass(0.7f);

    // Initialize LPF
    mTapLpfL.setLowpass(8000.0f, 0.707f);
    mTapLpfR.setLowpass(8000.0f, 0.707f);
}

void RiserReverbEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);

    mPreDelayL = DelayLine(200.0f, sr);
    mPreDelayR = DelayLine(200.0f, sr);
    mTapDelayL = DelayLine(1200.0f, sr);
    mTapDelayR = DelayLine(1200.0f, sr);

    mAttackSmooth.setSmoothingTime(30.0f, sampleRate);
    mSizeSmooth.setSmoothingTime(30.0f, sampleRate);
    mMixSmooth.setSmoothingTime(10.0f, sampleRate);
    mDampSmooth.setSmoothingTime(20.0f, sampleRate);

    // Re-setup filters
    float diff = mDiffusion.load(std::memory_order_relaxed);
    setupAllpass(diff);

    float damp = mDamping.load(std::memory_order_relaxed);
    float lpfCutoff = 16000.0f * std::pow(2000.0f / 16000.0f, damp);
    mTapLpfL.setLowpass(lpfCutoff, 0.707f);
    mTapLpfR.setLowpass(lpfCutoff, 0.707f);
}

void RiserReverbEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_ATTACK_TIME:
            mAttackTime.store(std::clamp(value, 100.0f, 3000.0f), std::memory_order_relaxed);
            break;
        case PARAM_DECAY:
            mDecay.store(std::clamp(value, 0.5f, 10.0f), std::memory_order_relaxed);
            break;
        case PARAM_SIZE:
            mSize.store(std::clamp(value, 0.1f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_DIFFUSION: {
            float clamped = std::clamp(value, 0.0f, 1.0f);
            mDiffusion.store(clamped, std::memory_order_relaxed);
            setupAllpass(clamped);
            break;
        }
        case PARAM_DAMPING: {
            float clamped = std::clamp(value, 0.0f, 1.0f);
            mDamping.store(clamped, std::memory_order_relaxed);
            float lpfCutoff = 16000.0f * std::pow(2000.0f / 16000.0f, clamped);
            mTapLpfL.setLowpass(lpfCutoff, 0.707f);
            mTapLpfR.setLowpass(lpfCutoff, 0.707f);
            break;
        }
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
    }
}

float RiserReverbEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_ATTACK_TIME: return mAttackTime.load(std::memory_order_relaxed);
        case PARAM_DECAY: return mDecay.load(std::memory_order_relaxed);
        case PARAM_SIZE: return mSize.load(std::memory_order_relaxed);
        case PARAM_DIFFUSION: return mDiffusion.load(std::memory_order_relaxed);
        case PARAM_DAMPING: return mDamping.load(std::memory_order_relaxed);
        case PARAM_MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void RiserReverbEffect::setupAllpass(float diffusion) {
    // Use cascaded LPFs for temporal smearing/diffusion
    // Lower cutoff = more smearing/diffusion
    float cutoff = 12000.0f * std::pow(3000.0f / 12000.0f, diffusion);
    cutoff = std::clamp(cutoff, 500.0f, 12000.0f);
    mDiffuseL1.setLowpass(cutoff, 0.5f);
    mDiffuseL2.setLowpass(cutoff * 0.7f, 0.5f);
    mDiffuseR1.setLowpass(cutoff, 0.5f);
    mDiffuseR2.setLowpass(cutoff * 0.7f, 0.5f);
}

void RiserReverbEffect::process(float* input, float* output, int numFrames) {
    float attackTime = mAttackTime.load(std::memory_order_relaxed);
    float decay = mDecay.load(std::memory_order_relaxed);
    float size = mSize.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);
    // damping is applied via the damping LPF coefficients, not read here.

    float sr = static_cast<float>(mSampleRate);

    float smoothAttack = mAttackSmooth.process(attackTime);

    // Pre-compute tap gains (rising envelope, scaled by decay)
    // Gains are computed per-block since they don't change per-sample
    float tapGains[NUM_TAPS];
    for (int t = 0; t < NUM_TAPS; ++t) {
        float tapTimeMs = TAP_BASE_MS[t] * size;
        float normalized = std::clamp(tapTimeMs / std::max(smoothAttack, 1.0f), 0.0f, 1.0f);
        float riseGain = normalized * normalized;
        // Decay factor: later taps decay more
        float decayGain = std::pow(10.0f, -3.0f * (tapTimeMs / 1000.0f) / decay);
        tapGains[t] = riseGain * decayGain;
    }

    // Load diffusion once per block (was inside inner loop)
    float diff = mDiffusion.load(std::memory_order_relaxed);

    for (int i = 0; i < numFrames; ++i) {
        float smoothSize = mSizeSmooth.process(size);
        float smoothMix = mMixSmooth.process(mix);

        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        // Pre-delay (fixed short pre-delay for riser effect)
        mPreDelayL.write(dryL);
        mPreDelayR.write(dryR);
        float preDelSamples = 10.0f * sr / 1000.0f;
        float preL = mPreDelayL.readInterpolated(preDelSamples);
        float preR = mPreDelayR.readInterpolated(preDelSamples);

        // Write to tap delay
        mTapDelayL.write(preL);
        mTapDelayR.write(preR);

        // Read taps with rising envelope
        float wetL = 0.0f, wetR = 0.0f;
        for (int t = 0; t < NUM_TAPS; ++t) {
            float tapMs = TAP_BASE_MS[t] * smoothSize;
            float tapSamples = tapMs * sr / 1000.0f;
            tapSamples = std::max(tapSamples, 1.0f);

            wetL += mTapDelayL.readInterpolated(tapSamples) * tapGains[t];
            wetR += mTapDelayR.readInterpolated(tapSamples) * tapGains[t];
        }

        // Normalize
        wetL *= (2.0f / NUM_TAPS);
        wetR *= (2.0f / NUM_TAPS);

        // Diffusion: cascaded LPF smearing
        float diffusedL = mDiffuseL2.process(mDiffuseL1.process(wetL));
        float diffusedR = mDiffuseR2.process(mDiffuseR1.process(wetR));
        wetL = wetL * (1.0f - diff) + diffusedL * diff;
        wetR = wetR * (1.0f - diff) + diffusedR * diff;

        // Damping LPF
        wetL = mTapLpfL.process(wetL);
        wetR = mTapLpfR.process(wetR);

        // Denormal protection
        if (std::abs(wetL) < 1e-20f) wetL = 0.0f;
        if (std::abs(wetR) < 1e-20f) wetR = 0.0f;

        // Mix
        float outL = dryL + (wetL - dryL) * smoothMix;
        float outR = dryR + (wetR - dryR) * smoothMix;

        // NaN/Inf protection
        if (!std::isfinite(outL)) outL = dryL;
        if (!std::isfinite(outR)) outR = dryR;

        output[i * 2]     = outL;
        output[i * 2 + 1] = outR;
    }
}
