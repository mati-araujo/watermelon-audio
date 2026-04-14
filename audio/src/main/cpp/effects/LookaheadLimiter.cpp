#include "LookaheadLimiter.h"
#include "EffectDefaults.h"
#include <cmath>
#include <algorithm>

LookaheadLimiter::LookaheadLimiter() {
    // Initialize with default sample rate
    prepare(DEFAULT_SAMPLE_RATE);
}

void LookaheadLimiter::prepare(int32_t sampleRate) {
    mSampleRate = sampleRate;

    // Calculate lookahead buffer size (stereo)
    mLookaheadSamples = static_cast<int32_t>(LOOKAHEAD_MS * sampleRate / 1000.0f);
    mDelayBuffer.resize(mLookaheadSamples * 2, 0.0f);  // Stereo
    mWritePos = 0;

    // Initialize gain to unity
    mGain = 1.0f;

    // Calculate envelope coefficients
    updateCoefficients();
}

void LookaheadLimiter::updateCoefficients() {
    // Convert threshold to linear
    mThresholdLinear = dbToLinear(mThresholdDb.load(std::memory_order_relaxed));

    // Calculate one-pole filter coefficients for envelope
    // Attack: fast response to catch transients
    float attackMs = mAttackMs.load(std::memory_order_relaxed);
    mAttackCoeff = std::exp(-1.0f / (attackMs * mSampleRate / 1000.0f));

    // Release: smooth recovery to avoid pumping
    float releaseMs = mReleaseMs.load(std::memory_order_relaxed);
    mReleaseCoeff = std::exp(-1.0f / (releaseMs * mSampleRate / 1000.0f));
}

void LookaheadLimiter::process(float* input, float* output, int numFrames) {
    // Cache threshold locally for RT thread
    const float threshold = mThresholdLinear;
    const float attackCoeff = mAttackCoeff;
    const float releaseCoeff = mReleaseCoeff;

    // Denormal protection threshold
    constexpr float DENORMAL_THRESHOLD = 1e-20f;

    for (int i = 0; i < numFrames; ++i) {
        // Get input samples (stereo interleaved)
        float inL = input[i * 2];
        float inR = input[i * 2 + 1];

        // Read delayed signal from lookahead buffer
        int readPos = (mWritePos - mLookaheadSamples + static_cast<int>(mDelayBuffer.size() / 2));
        if (readPos < 0) readPos += static_cast<int>(mDelayBuffer.size() / 2);
        readPos = readPos % (static_cast<int>(mDelayBuffer.size() / 2));

        float delayedL = mDelayBuffer[readPos * 2];
        float delayedR = mDelayBuffer[readPos * 2 + 1];

        // Write current input to delay buffer
        mDelayBuffer[mWritePos * 2] = inL;
        mDelayBuffer[mWritePos * 2 + 1] = inR;
        mWritePos = (mWritePos + 1) % (static_cast<int>(mDelayBuffer.size() / 2));

        // Detect peak level of input (for lookahead detection)
        float peak = std::max(std::abs(inL), std::abs(inR));

        // Calculate target gain to stay under threshold
        float targetGain = 1.0f;
        if (peak > threshold) {
            targetGain = threshold / peak;
        }

        // Smooth gain changes with attack/release envelope
        if (targetGain < mGain) {
            // Attack: gain is decreasing (limiting engaged)
            mGain = attackCoeff * mGain + (1.0f - attackCoeff) * targetGain;
        } else {
            // Release: gain is recovering
            mGain = releaseCoeff * mGain + (1.0f - releaseCoeff) * targetGain;
        }

        // Denormal protection on gain
        if (mGain < DENORMAL_THRESHOLD) mGain = 0.0f;
        if (mGain > 1.0f) mGain = 1.0f;  // Never amplify

        // Apply gain to delayed signal
        float outL = delayedL * mGain;
        float outR = delayedR * mGain;

        // Final safety clamp (should rarely trigger with proper threshold)
        outL = std::clamp(outL, -1.0f, 1.0f);
        outR = std::clamp(outR, -1.0f, 1.0f);

        output[i * 2] = outL;
        output[i * 2 + 1] = outR;
    }

    // Update metering (gain reduction in dB, positive value)
    float reductionDb = -linearToDb(mGain);
    if (reductionDb < 0.0f) reductionDb = 0.0f;
    mCurrentGainReduction.store(reductionDb, std::memory_order_relaxed);
}

void LookaheadLimiter::setParam(int paramId, float value) {
    switch (paramId) {
        case 0:
            setThreshold(value);
            break;
        case 1:
            setAttack(value);
            break;
        case 2:
            setRelease(value);
            break;
        default:
            break;
    }
}

float LookaheadLimiter::getParam(int paramId) {
    switch (paramId) {
        case 0:
            return mThresholdDb.load(std::memory_order_relaxed);
        case 1:
            return mAttackMs.load(std::memory_order_relaxed);
        case 2:
            return mReleaseMs.load(std::memory_order_relaxed);
        default:
            return 0.0f;
    }
}

void LookaheadLimiter::setSampleRate(int sampleRate) {
    prepare(sampleRate);
}

void LookaheadLimiter::setThreshold(float thresholdDb) {
    thresholdDb = std::clamp(thresholdDb, MIN_THRESHOLD_DB, MAX_THRESHOLD_DB);
    mThresholdDb.store(thresholdDb, std::memory_order_relaxed);
    updateCoefficients();
}

void LookaheadLimiter::setAttack(float attackMs) {
    attackMs = std::clamp(attackMs, MIN_ATTACK_MS, MAX_ATTACK_MS);
    mAttackMs.store(attackMs, std::memory_order_relaxed);
    updateCoefficients();
}

void LookaheadLimiter::setRelease(float releaseMs) {
    releaseMs = std::clamp(releaseMs, MIN_RELEASE_MS, MAX_RELEASE_MS);
    mReleaseMs.store(releaseMs, std::memory_order_relaxed);
    updateCoefficients();
}

float LookaheadLimiter::getGainReduction() const {
    return mCurrentGainReduction.load(std::memory_order_relaxed);
}
