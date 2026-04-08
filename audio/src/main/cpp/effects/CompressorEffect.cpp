#include "CompressorEffect.h"
#include <algorithm>
#include "../platform/Logger.h"

#define LOG_TAG "CompressorEffect"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)

CompressorEffect::CompressorEffect() {
    updateCoefficients();
    LOGI("CompressorEffect created");
}

void CompressorEffect::process(float* input, float* output, int numFrames) {
    const float threshold = mThresholdDb.load(std::memory_order_relaxed);
    const float ratio = mRatio.load(std::memory_order_relaxed);
    const float knee = mKneeDb.load(std::memory_order_relaxed);
    const float makeupTarget = std::pow(10.0f, mMakeupDb.load(std::memory_order_relaxed) / 20.0f);

    float maxGainReduction = 0.0f;

    for (int i = 0; i < numFrames; ++i) {
        const int idx = i * 2;

        // Smooth makeup gain per-sample to prevent clicks
        float makeupLinear = mMakeupSmoother.process(makeupTarget);

        // Get peak of stereo pair
        float inputPeak = std::max(std::abs(input[idx]), std::abs(input[idx + 1]));

        // Convert to dB
        float inputDb = (inputPeak > 1e-6f) ? 20.0f * std::log10(inputPeak) : -120.0f;

        // Compute target gain reduction
        float targetGainDb = computeGain(inputDb);

        // Envelope follower (peak detector with attack/release)
        if (targetGainDb < mEnvelope) {
            // Attack
            mEnvelope = mAttackCoeff * mEnvelope + (1.0f - mAttackCoeff) * targetGainDb;
        } else {
            // Release
            mEnvelope = mReleaseCoeff * mEnvelope + (1.0f - mReleaseCoeff) * targetGainDb;
        }

        // Convert to linear and apply makeup
        float gainLinear = std::pow(10.0f, mEnvelope / 20.0f) * makeupLinear;

        // Apply gain
        output[idx] = input[idx] * gainLinear;
        output[idx + 1] = input[idx + 1] * gainLinear;

        // Track gain reduction for metering
        maxGainReduction = std::min(maxGainReduction, mEnvelope);
    }

    mGainReductionDb.store(maxGainReduction, std::memory_order_relaxed);
}

float CompressorEffect::computeGain(float inputDb) const {
    const float threshold = mThresholdDb.load(std::memory_order_relaxed);
    const float ratio = mRatio.load(std::memory_order_relaxed);
    const float knee = mKneeDb.load(std::memory_order_relaxed);

    // Below threshold
    if (inputDb < threshold - knee / 2.0f) {
        return 0.0f;  // No gain reduction
    }

    // Above threshold (hard knee)
    if (inputDb > threshold + knee / 2.0f || knee < 0.1f) {
        float excess = inputDb - threshold;
        return excess * (1.0f / ratio - 1.0f);  // Negative = reduction
    }

    // In knee region (soft knee)
    float kneeInput = inputDb - (threshold - knee / 2.0f);
    float kneeGain = (kneeInput * kneeInput) / (2.0f * knee);
    return kneeGain * (1.0f / ratio - 1.0f);
}

void CompressorEffect::updateCoefficients() {
    float attackSec = mAttackMs.load(std::memory_order_relaxed) / 1000.0f;
    float releaseSec = mReleaseMs.load(std::memory_order_relaxed) / 1000.0f;

    mAttackCoeff = std::exp(-1.0f / (attackSec * static_cast<float>(mSampleRate)));
    mReleaseCoeff = std::exp(-1.0f / (releaseSec * static_cast<float>(mSampleRate)));
}

void CompressorEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case THRESHOLD:
            mThresholdDb.store(std::clamp(value, -60.0f, 0.0f), std::memory_order_relaxed);
            break;
        case RATIO:
            mRatio.store(std::clamp(value, 1.0f, 20.0f), std::memory_order_relaxed);
            break;
        case ATTACK:
            mAttackMs.store(std::clamp(value, 0.1f, 100.0f), std::memory_order_relaxed);
            updateCoefficients();
            break;
        case RELEASE:
            mReleaseMs.store(std::clamp(value, 10.0f, 1000.0f), std::memory_order_relaxed);
            updateCoefficients();
            break;
        case MAKEUP_GAIN:
            mMakeupDb.store(std::clamp(value, -6.0f, 24.0f), std::memory_order_relaxed);
            break;
        case KNEE:
            mKneeDb.store(std::clamp(value, 0.0f, 20.0f), std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

float CompressorEffect::getParam(int paramId) {
    switch (paramId) {
        case THRESHOLD: return mThresholdDb.load(std::memory_order_relaxed);
        case RATIO: return mRatio.load(std::memory_order_relaxed);
        case ATTACK: return mAttackMs.load(std::memory_order_relaxed);
        case RELEASE: return mReleaseMs.load(std::memory_order_relaxed);
        case MAKEUP_GAIN: return mMakeupDb.load(std::memory_order_relaxed);
        case KNEE: return mKneeDb.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void CompressorEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    updateCoefficients();
    mMakeupSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    LOGI("Sample rate set to %d", sampleRate);
}
