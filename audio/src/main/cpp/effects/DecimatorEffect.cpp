#include "DecimatorEffect.h"

DecimatorEffect::DecimatorEffect() {
    mBitDepthSmooth.reset(16.0f);
    mSampleRateSmooth.reset(48000.0f);
    mMixSmooth.reset(1.0f);
}

void DecimatorEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);
    mBitDepthSmooth.setSmoothingTime(10.0f, sr);
    mSampleRateSmooth.setSmoothingTime(10.0f, sr);
    mMixSmooth.setSmoothingTime(5.0f, sr);
}

void DecimatorEffect::reset() {
    // El hold guarda la ultima muestra retenida y su contador fraccionario:
    // sin limpiarlo, el primer bloque del contexto nuevo arranca repitiendo una
    // muestra del anterior.
    mHoldL = 0.0f;
    mHoldR = 0.0f;
    mHoldCounter = 0.0f;

    mBitDepthSmooth.reset(mBitDepth.load(std::memory_order_relaxed));
    mSampleRateSmooth.reset(mTargetSampleRate.load(std::memory_order_relaxed));
    mMixSmooth.reset(mMix.load(std::memory_order_relaxed));
}

void DecimatorEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_BIT_DEPTH:
            mBitDepth.store(std::clamp(value, 1.0f, 24.0f), std::memory_order_relaxed);
            break;
        case PARAM_SAMPLE_RATE:
            mTargetSampleRate.store(std::clamp(value, 100.0f, 48000.0f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
    }
}

float DecimatorEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_BIT_DEPTH:   return mBitDepth.load(std::memory_order_relaxed);
        case PARAM_SAMPLE_RATE: return mTargetSampleRate.load(std::memory_order_relaxed);
        case PARAM_MIX:         return mMix.load(std::memory_order_relaxed);
        default:                return 0.0f;
    }
}

void DecimatorEffect::process(float* input, float* output, int numFrames) {
    float bitDepth = mBitDepth.load(std::memory_order_relaxed);
    float targetSR = mTargetSampleRate.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);

    for (int i = 0; i < numFrames; ++i) {
        float smoothBits = mBitDepthSmooth.process(bitDepth);
        float smoothSR = mSampleRateSmooth.process(targetSR);
        float smoothMix = mMixSmooth.process(mix);

        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        // Sample rate reduction: zero-order hold
        float step = static_cast<float>(mSampleRate) / std::max(smoothSR, 100.0f);
        mHoldCounter += 1.0f;
        if (mHoldCounter >= step) {
            mHoldCounter -= step;
            mHoldL = dryL;
            mHoldR = dryR;
        }

        // Bit depth reduction: quantization
        // Supports fractional bit values for smooth transitions
        float levels = std::pow(2.0f, smoothBits);
        float wetL = std::floor(mHoldL * levels) / levels;
        float wetR = std::floor(mHoldR * levels) / levels;

        // Mix dry/wet
        output[i * 2]     = dryL + (wetL - dryL) * smoothMix;
        output[i * 2 + 1] = dryR + (wetR - dryR) * smoothMix;
    }
}
