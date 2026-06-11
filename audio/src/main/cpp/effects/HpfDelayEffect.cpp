#include "HpfDelayEffect.h"

namespace {
float subdivisionToMs(int subdivision, float bpm) {
    float quarterMs = 60000.0f / bpm;
    switch (std::clamp(subdivision, 0, 5)) {
        case 0: return quarterMs;             // 1/4
        case 1: return quarterMs * 0.5f;      // 1/8
        case 2: return quarterMs * 0.75f;     // dotted 1/8
        case 3: return quarterMs / 3.0f;      // 1/8 triplet
        case 4: return quarterMs * 0.25f;     // 1/16
        case 5: return quarterMs * 1.5f;      // dotted 1/4
        default: return quarterMs * 0.5f;
    }
}
}

void HpfDelayEffect::reset() {
    // Clear delay lines (which hold the feedback echo path) and
    // the HPF state (which holds the last-sample filter memory).
    mDelayL.clear();
    mDelayR.clear();
    mHpfL.reset();
    mHpfR.reset();
    mLpfL.reset();
    mLpfR.reset();
}

HpfDelayEffect::HpfDelayEffect()
    : mDelayL(2000.0f, 48000.0f),   // 2 second max delay
      mDelayR(2000.0f, 48000.0f),
      mHpfL(48000.0f),
      mHpfR(48000.0f),
      mLpfL(48000.0f),
      mLpfR(48000.0f) {
    mHpfL.setHighpass(200.0f, 0.707f);
    mHpfR.setHighpass(200.0f, 0.707f);
    mLpfL.setLowpass(12000.0f, 0.707f);
    mLpfR.setLowpass(12000.0f, 0.707f);

    mDelaySmooth.reset(300.0f);
    mFeedbackSmooth.reset(0.4f);
    mMixSmooth.reset(0.5f);
}

void HpfDelayEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);

    mDelayL.setSampleRate(sr);
    mDelayR.setSampleRate(sr);
    mDelayL.setMaxDelay(2000.0f);
    mDelayR.setMaxDelay(2000.0f);

    mHpfL.setSampleRate(sr);
    mHpfR.setSampleRate(sr);
    mLpfL.setSampleRate(sr);
    mLpfR.setSampleRate(sr);

    mDelaySmooth.setSmoothingTime(10.0f, sr);
    mFeedbackSmooth.setSmoothingTime(10.0f, sr);
    mMixSmooth.setSmoothingTime(5.0f, sr);
}

void HpfDelayEffect::setBpm(float bpm) {
    mBpm.store(std::clamp(bpm, 20.0f, 300.0f), std::memory_order_relaxed);
}

void HpfDelayEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_HPF_CUTOFF:
            mHpfCutoff.store(std::clamp(value, 20.0f, 8000.0f), std::memory_order_relaxed);
            break;
        case PARAM_DELAY_TIME:
            mDelayTime.store(std::clamp(value, 10.0f, 2000.0f), std::memory_order_relaxed);
            break;
        case PARAM_FEEDBACK:
            mFeedback.store(std::clamp(value, 0.0f, 0.95f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_SYNC:
            mSync.store(value > 0.5f, std::memory_order_relaxed);
            break;
        case PARAM_SUBDIVISION:
            mSubdivision.store(std::clamp(static_cast<int>(value), 0, 5), std::memory_order_relaxed);
            break;
        case PARAM_PING_PONG:
            mPingPong.store(value > 0.5f, std::memory_order_relaxed);
            break;
        case PARAM_DUCKING:
            mDucking.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_LPF_CUTOFF:
            mLpfCutoff.store(std::clamp(value, 1000.0f, 20000.0f), std::memory_order_relaxed);
            break;
    }
}

float HpfDelayEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_HPF_CUTOFF: return mHpfCutoff.load(std::memory_order_relaxed);
        case PARAM_DELAY_TIME: return mDelayTime.load(std::memory_order_relaxed);
        case PARAM_FEEDBACK:   return mFeedback.load(std::memory_order_relaxed);
        case PARAM_MIX:        return mMix.load(std::memory_order_relaxed);
        case PARAM_SYNC:       return mSync.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        case PARAM_SUBDIVISION:return static_cast<float>(mSubdivision.load(std::memory_order_relaxed));
        case PARAM_PING_PONG:  return mPingPong.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        case PARAM_DUCKING:    return mDucking.load(std::memory_order_relaxed);
        case PARAM_LPF_CUTOFF: return mLpfCutoff.load(std::memory_order_relaxed);
        default:               return 0.0f;
    }
}

void HpfDelayEffect::process(float* input, float* output, int numFrames) {
    float hpfCutoff = mHpfCutoff.load(std::memory_order_relaxed);
    float delayMs = mSync.load(std::memory_order_relaxed)
        ? subdivisionToMs(mSubdivision.load(std::memory_order_relaxed),
                          mBpm.load(std::memory_order_relaxed))
        : mDelayTime.load(std::memory_order_relaxed);
    float feedback = mFeedback.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);
    float ducking = mDucking.load(std::memory_order_relaxed);
    bool pingPong = mPingPong.load(std::memory_order_relaxed);
    float lpfCutoff = mLpfCutoff.load(std::memory_order_relaxed);

    // Update HPF cutoff only if changed (avoid redundant coefficient recalculation)
    if (std::abs(hpfCutoff - mLastHpfCutoff) > 0.5f) {
        mHpfL.setHighpass(hpfCutoff, 0.707f);
        mHpfR.setHighpass(hpfCutoff, 0.707f);
        mLastHpfCutoff = hpfCutoff;
    }
    if (std::abs(lpfCutoff - mLastLpfCutoff) > 2.0f) {
        mLpfL.setLowpass(lpfCutoff, 0.707f);
        mLpfR.setLowpass(lpfCutoff, 0.707f);
        mLastLpfCutoff = lpfCutoff;
    }

    constexpr float DENORMAL_THRESHOLD = 1e-20f;

    for (int i = 0; i < numFrames; ++i) {
        float smoothDelay = mDelaySmooth.process(delayMs);
        float smoothFb = mFeedbackSmooth.process(feedback);
        float smoothMix = mMixSmooth.process(mix);

        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        // Read from delay line
        float delayedL = mDelayL.readMs(smoothDelay);
        float delayedR = mDelayR.readMs(smoothDelay);

        // HPF in feedback path: each repetition loses low frequencies
        float fbL = mLpfL.process(mHpfL.process(delayedL)) * smoothFb;
        float fbR = mLpfR.process(mHpfR.process(delayedR)) * smoothFb;

        // Denormal protection on feedback
        if (std::abs(fbL) < DENORMAL_THRESHOLD) fbL = 0.0f;
        if (std::abs(fbR) < DENORMAL_THRESHOLD) fbR = 0.0f;

        if (pingPong) {
            mDelayL.write(dryL + fbR);
            mDelayR.write(dryR + fbL);
        } else {
            mDelayL.write(dryL + fbL);
            mDelayR.write(dryR + fbR);
        }

        // Output: dry/wet mix
        float inputLevel = std::clamp((std::abs(dryL) + std::abs(dryR)) * 0.5f * 2.0f, 0.0f, 1.0f);
        float duckGain = 1.0f - ducking * inputLevel;
        output[i * 2]     = dryL + (delayedL * duckGain - dryL) * smoothMix;
        output[i * 2 + 1] = dryR + (delayedR * duckGain - dryR) * smoothMix;
    }
}
