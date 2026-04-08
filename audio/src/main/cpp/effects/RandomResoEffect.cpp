#include "RandomResoEffect.h"

RandomResoEffect::RandomResoEffect()
    : mFilterL(48000.0f),
      mFilterR(48000.0f),
      mRandomLfo(48000.0f, 2.0f) {
    mRandomLfo.setWaveform(LFO::Waveform::RANDOM_SMOOTH);
    mMixSmooth.reset(1.0f);

    // Initialize filters with default BPF settings
    mFilterL.setBandpass(1000.0f, 8.0f);
    mFilterR.setBandpass(1000.0f, 8.0f);
}

void RandomResoEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);

    mFilterL.setSampleRate(sr);
    mFilterR.setSampleRate(sr);
    mRandomLfo.setSampleRate(sr);

    mMixSmooth.setSmoothingTime(5.0f, sr);
}

void RandomResoEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_CENTER_FREQ:
            mCenterFreq.store(std::clamp(value, 80.0f, 12000.0f), std::memory_order_relaxed);
            break;
        case PARAM_RESONANCE:
            mResonance.store(std::clamp(value, 0.5f, 30.0f), std::memory_order_relaxed);
            break;
        case PARAM_LFO_RATE:
            mLfoRate.store(std::clamp(value, 0.1f, 20.0f), std::memory_order_relaxed);
            break;
        case PARAM_LFO_DEPTH:
            mLfoDepth.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
    }
}

float RandomResoEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_CENTER_FREQ: return mCenterFreq.load(std::memory_order_relaxed);
        case PARAM_RESONANCE:   return mResonance.load(std::memory_order_relaxed);
        case PARAM_LFO_RATE:    return mLfoRate.load(std::memory_order_relaxed);
        case PARAM_LFO_DEPTH:   return mLfoDepth.load(std::memory_order_relaxed);
        case PARAM_MIX:         return mMix.load(std::memory_order_relaxed);
        default:                return 0.0f;
    }
}

void RandomResoEffect::process(float* input, float* output, int numFrames) {
    float centerFreq = mCenterFreq.load(std::memory_order_relaxed);
    float resonance = mResonance.load(std::memory_order_relaxed);
    float lfoRate = mLfoRate.load(std::memory_order_relaxed);
    float lfoDepth = mLfoDepth.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);

    mRandomLfo.setRate(lfoRate);

    // Sub-block processing: recalculate filter coefficients every SUB_BLOCK samples
    for (int block = 0; block < numFrames; block += SUB_BLOCK) {
        int blockSize = std::min(SUB_BLOCK, numFrames - block);

        // Advance LFO once per sub-block
        float lfoVal = mRandomLfo.process();  // [-1, +1]

        // Modulate in logarithmic scale (octaves)
        float modOctaves = lfoVal * lfoDepth * 4.0f;  // max 4 octaves
        float modulatedFreq = centerFreq * std::pow(2.0f, modOctaves);

        // Clamp to audible range
        modulatedFreq = std::clamp(modulatedFreq, 20.0f, 20000.0f);

        // Update BPF coefficients for this sub-block
        mFilterL.setBandpass(modulatedFreq, resonance);
        mFilterR.setBandpass(modulatedFreq, resonance);

        // Process sub-block
        for (int i = 0; i < blockSize; ++i) {
            int idx = (block + i) * 2;
            float smoothMix = mMixSmooth.process(mix);

            float dryL = input[idx];
            float dryR = input[idx + 1];

            float wetL = mFilterL.process(dryL);
            float wetR = mFilterR.process(dryR);

            // High-Q resonant BPF can produce extreme peaks — soft clamp
            wetL = std::clamp(wetL, -4.0f, 4.0f);
            wetR = std::clamp(wetR, -4.0f, 4.0f);

            // NaN/Inf protection
            if (!std::isfinite(wetL)) wetL = 0.0f;
            if (!std::isfinite(wetR)) wetR = 0.0f;

            output[idx]     = dryL + (wetL - dryL) * smoothMix;
            output[idx + 1] = dryR + (wetR - dryR) * smoothMix;
        }
    }
}
