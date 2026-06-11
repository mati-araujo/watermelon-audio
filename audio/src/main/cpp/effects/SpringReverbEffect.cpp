#include "SpringReverbEffect.h"
#include <algorithm>
#include <cmath>

SpringReverbEffect::SpringReverbEffect()
    : mTankL(900.0f),
      mTankR(900.0f),
      mToneL(48000.0f),
      mToneR(48000.0f),
      mInputHpfL(48000.0f),
      mInputHpfR(48000.0f) {
    mToneL.setBandpass(2200.0f, 0.7f);
    mToneR.setBandpass(2200.0f, 0.7f);
    mInputHpfL.setHighpass(120.0f, 0.707f);
    mInputHpfR.setHighpass(120.0f, 0.707f);
    mMixSmooth.reset(0.25f);
    mDecaySmooth.reset(2.2f);
}

void SpringReverbEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);
    mTankL.setSampleRate(sr);
    mTankR.setSampleRate(sr);
    mTankL.setMaxDelay(900.0f);
    mTankR.setMaxDelay(900.0f);
    mToneL.setSampleRate(sr);
    mToneR.setSampleRate(sr);
    mInputHpfL.setSampleRate(sr);
    mInputHpfR.setSampleRate(sr);
    mInputHpfL.setHighpass(120.0f, 0.707f);
    mInputHpfR.setHighpass(120.0f, 0.707f);
    mMixSmooth.setSmoothingTime(10.0f, sr);
    mDecaySmooth.setSmoothingTime(20.0f, sr);
}

void SpringReverbEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_DECAY: mDecay.store(std::clamp(value, 0.4f, 5.0f), std::memory_order_relaxed); break;
        case PARAM_TONE: mTone.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case PARAM_DRIP: mDrip.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case PARAM_TENSION: mTension.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case PARAM_MIX: mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        default: break;
    }
}

float SpringReverbEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_DECAY: return mDecay.load(std::memory_order_relaxed);
        case PARAM_TONE: return mTone.load(std::memory_order_relaxed);
        case PARAM_DRIP: return mDrip.load(std::memory_order_relaxed);
        case PARAM_TENSION: return mTension.load(std::memory_order_relaxed);
        case PARAM_MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void SpringReverbEffect::process(float* input, float* output, int numFrames) {
    float tone = mTone.load(std::memory_order_relaxed);
    if (std::abs(tone - mLastTone) > 0.01f) {
        float freq = 900.0f + tone * 4200.0f;
        mToneL.setBandpass(freq, 0.55f + tone * 1.2f);
        mToneR.setBandpass(freq * 1.03f, 0.55f + tone * 1.2f);
        mLastTone = tone;
    }

    float drip = mDrip.load(std::memory_order_relaxed);
    float tension = mTension.load(std::memory_order_relaxed);
    float mixTarget = mMix.load(std::memory_order_relaxed);
    float decayTarget = mDecay.load(std::memory_order_relaxed);

    for (int i = 0; i < numFrames; ++i) {
        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];
        float mix = mMixSmooth.process(mixTarget);
        float decay = mDecaySmooth.process(decayTarget);

        float inL = mInputHpfL.process(dryL);
        float inR = mInputHpfR.process(dryR);

        float stretch = 0.75f + tension * 0.45f;
        float tapL =
            mTankL.readMs(23.0f * stretch) * 0.55f +
            mTankL.readMs(47.0f * stretch) * 0.42f +
            mTankL.readMs(89.0f * stretch) * 0.30f +
            mTankL.readMs(151.0f * stretch) * 0.22f;
        float tapR =
            mTankR.readMs(29.0f * stretch) * 0.55f +
            mTankR.readMs(53.0f * stretch) * 0.42f +
            mTankR.readMs(97.0f * stretch) * 0.30f +
            mTankR.readMs(167.0f * stretch) * 0.22f;

        float feedback = std::clamp(0.45f + decay * 0.11f, 0.0f, 0.92f);
        float dripClickL = mTankL.readMs(7.0f) * drip * 0.45f;
        float dripClickR = mTankR.readMs(9.0f) * drip * 0.45f;

        mTankL.write(inL + tapR * feedback + dripClickR);
        mTankR.write(inR + tapL * feedback + dripClickL);

        float wetL = mToneL.process(tapL + dripClickL);
        float wetR = mToneR.process(tapR + dripClickR);

        if (!std::isfinite(wetL)) wetL = 0.0f;
        if (!std::isfinite(wetR)) wetR = 0.0f;

        output[i * 2] = dryL + (wetL - dryL) * mix;
        output[i * 2 + 1] = dryR + (wetR - dryR) * mix;
    }
}

void SpringReverbEffect::reset() {
    mTankL.clear();
    mTankR.clear();
    mToneL.reset();
    mToneR.reset();
    mInputHpfL.reset();
    mInputHpfR.reset();
}
