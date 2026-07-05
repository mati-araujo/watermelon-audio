#include "ShimmerReverbEffect.h"
#include <algorithm>
#include <cmath>

ShimmerReverbEffect::ShimmerReverbEffect()
    : mToneL(48000.0f),
      mToneR(48000.0f) {
    mPitchGrains.setSampleRate(48000);
    mFdn.setDecayTime(5.0f);
    mFdn.setSize(0.85f);
    mFdn.setDamping(0.25f, 0.35f);
    mFdn.setModulation(0.25f);
    mToneL.setLowpass(10500.0f, 0.707f);
    mToneR.setLowpass(10500.0f, 0.707f);
    mMixSmooth.reset(0.35f);
    mShimmerSmooth.reset(0.35f);
}

void ShimmerReverbEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    mPitchGrains.setSampleRate(sampleRate);
    mFdn.setSampleRate(sampleRate);
    mToneL.setSampleRate(static_cast<float>(sampleRate));
    mToneR.setSampleRate(static_cast<float>(sampleRate));
    mMixSmooth.setSmoothingTime(20.0f, static_cast<float>(sampleRate));
    mShimmerSmooth.setSmoothingTime(30.0f, static_cast<float>(sampleRate));
}

void ShimmerReverbEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_DECAY:
            mDecay.store(std::clamp(value, 1.0f, 15.0f), std::memory_order_relaxed);
            mFdn.setDecayTime(std::clamp(value, 1.0f, 15.0f));
            break;
        case PARAM_SIZE:
            mSize.store(std::clamp(value, 0.1f, 1.0f), std::memory_order_relaxed);
            mFdn.setSize(std::clamp(value, 0.1f, 1.0f));
            break;
        case PARAM_PITCH_SEMITONES:
            mPitchSemitones.store(std::clamp(value, -12.0f, 24.0f), std::memory_order_relaxed);
            break;
        case PARAM_SHIMMER_AMOUNT:
            mShimmerAmount.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_FEEDBACK:
            mFeedback.store(std::clamp(value, 0.0f, 0.85f), std::memory_order_relaxed);
            break;
        case PARAM_TONE:
            mTone.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

float ShimmerReverbEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_DECAY: return mDecay.load(std::memory_order_relaxed);
        case PARAM_SIZE: return mSize.load(std::memory_order_relaxed);
        case PARAM_PITCH_SEMITONES: return mPitchSemitones.load(std::memory_order_relaxed);
        case PARAM_SHIMMER_AMOUNT: return mShimmerAmount.load(std::memory_order_relaxed);
        case PARAM_FEEDBACK: return mFeedback.load(std::memory_order_relaxed);
        case PARAM_TONE: return mTone.load(std::memory_order_relaxed);
        case PARAM_MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void ShimmerReverbEffect::process(float* input, float* output, int numFrames) {
    float tone = mTone.load(std::memory_order_relaxed);
    if (std::abs(tone - mLastTone) > 0.01f) {
        float cutoff = 4500.0f + tone * 11500.0f;
        mToneL.setLowpass(cutoff, 0.707f);
        mToneR.setLowpass(cutoff, 0.707f);
        mLastTone = tone;
    }

    float mixTarget = mMix.load(std::memory_order_relaxed);
    float shimmerTarget = mShimmerAmount.load(std::memory_order_relaxed);
    float pitch = mPitchSemitones.load(std::memory_order_relaxed);
    float feedback = mFeedback.load(std::memory_order_relaxed);
    int grainInterval = std::max(1, static_cast<int>(0.025f * static_cast<float>(mSampleRate)));

    for (int i = 0; i < numFrames; ++i) {
        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];
        float shimmer = mShimmerSmooth.process(shimmerTarget);
        float mix = mMixSmooth.process(mixTarget);

        float grainInL = dryL + mFeedbackL * feedback;
        float grainInR = dryR + mFeedbackR * feedback;
        mPitchGrains.writeToBuffer(grainInL, grainInR);

        if (--mGrainCounter <= 0) {
            mGrainCounter = grainInterval;
            mPitchGrains.triggerGrain(80.0f, 0.18f, pitch);
        }

        float pitchL = 0.0f;
        float pitchR = 0.0f;
        mPitchGrains.process(pitchL, pitchR);

        float fdnInL = dryL + pitchL * shimmer;
        float fdnInR = dryR + pitchR * shimmer;

        float wetL = 0.0f;
        float wetR = 0.0f;
        mFdn.process(fdnInL, fdnInR, wetL, wetR);
        wetL = mToneL.process(wetL);
        wetR = mToneR.process(wetR);

        if (!std::isfinite(wetL)) wetL = 0.0f;
        if (!std::isfinite(wetR)) wetR = 0.0f;

        mFeedbackL = std::clamp(wetL, -1.5f, 1.5f);
        mFeedbackR = std::clamp(wetR, -1.5f, 1.5f);

        output[i * 2] = dryL + (wetL - dryL) * mix;
        output[i * 2 + 1] = dryR + (wetR - dryR) * mix;
    }
}

void ShimmerReverbEffect::reset() {
    mFdn.reset();
    mPitchGrains.reset();
    mToneL.reset();
    mToneR.reset();
    mFeedbackL = 0.0f;
    mFeedbackR = 0.0f;
    mGrainCounter = 0;
}
