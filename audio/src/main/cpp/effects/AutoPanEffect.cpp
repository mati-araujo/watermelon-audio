#include "AutoPanEffect.h"

AutoPanEffect::AutoPanEffect() : mLfo(48000.0f, 2.0f) {
    mRateSmooth.reset(2.0f);
    mDepthSmooth.reset(0.8f);
    mMixSmooth.reset(1.0f);
}

void AutoPanEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);
    mLfo.setSampleRate(sr);
    mRateSmooth.setSmoothingTime(10.0f, sr);
    mDepthSmooth.setSmoothingTime(10.0f, sr);
    mMixSmooth.setSmoothingTime(5.0f, sr);
}

void AutoPanEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_RATE:
            mRate.store(std::clamp(value, 0.1f, 20.0f), std::memory_order_relaxed);
            break;
        case PARAM_DEPTH:
            mDepth.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_WAVEFORM:
            mWaveform.store(std::clamp(static_cast<int>(value), 0, 2), std::memory_order_relaxed);
            break;
        case PARAM_PHASE_OFFSET: {
            float clamped = std::clamp(value, 0.0f, 360.0f);
            mPhaseOffset.store(clamped, std::memory_order_relaxed);
            // Convert degrees to radians for LFO
            mLfo.setPhaseOffset(clamped * DSPMath::PI / 180.0f);
            break;
        }
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
    }
}

float AutoPanEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_RATE:         return mRate.load(std::memory_order_relaxed);
        case PARAM_DEPTH:        return mDepth.load(std::memory_order_relaxed);
        case PARAM_WAVEFORM:     return static_cast<float>(mWaveform.load(std::memory_order_relaxed));
        case PARAM_PHASE_OFFSET: return mPhaseOffset.load(std::memory_order_relaxed);
        case PARAM_MIX:          return mMix.load(std::memory_order_relaxed);
        default:                 return 0.0f;
    }
}

void AutoPanEffect::process(float* input, float* output, int numFrames) {
    float rate = mRate.load(std::memory_order_relaxed);
    float depth = mDepth.load(std::memory_order_relaxed);
    int waveform = mWaveform.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);

    // Update LFO settings per-block (not per-sample for efficiency)
    mLfo.setRate(rate);
    static constexpr LFO::Waveform waveforms[] = {
        LFO::Waveform::SINE,
        LFO::Waveform::TRIANGLE,
        LFO::Waveform::SQUARE
    };
    int safeWaveform = std::clamp(waveform, 0, 2);
    mLfo.setWaveform(waveforms[safeWaveform]);

    for (int i = 0; i < numFrames; ++i) {
        float smoothDepth = mDepthSmooth.process(depth);
        float smoothMix = mMixSmooth.process(mix);

        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        // LFO output [-1, +1], scaled by depth
        float lfoVal = mLfo.process();
        float pan = lfoVal * smoothDepth;

        // Equal-power panning gains
        float gainL = DSPMath::panGainL(pan);
        float gainR = DSPMath::panGainR(pan);

        // Sum to mono then apply pan
        float mono = (dryL + dryR) * 0.5f;
        float wetL = mono * gainL;
        float wetR = mono * gainR;

        // Mix
        output[i * 2]     = dryL + (wetL - dryL) * smoothMix;
        output[i * 2 + 1] = dryR + (wetR - dryR) * smoothMix;
    }
}
