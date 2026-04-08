#include "TapeEchoEffect.h"
#include <cmath>
#include <algorithm>

TapeEchoEffect::TapeEchoEffect()
    : mDelayL(static_cast<int>(48000 * 2.5f)),
      mDelayR(static_cast<int>(48000 * 2.5f)) {
    // Wow: slow sine LFO
    mWowLfo.setWaveform(LFO::Waveform::SINE);
    mWowLfo.setRate(1.5f);

    // Flutter: fast random-smooth LFO
    mFlutterLfo.setWaveform(LFO::Waveform::RANDOM_SMOOTH);
    mFlutterLfo.setRate(6.0f);

    // Initialize smoothers with defaults
    mDelaySmooth.reset(350.0f);
    mFeedbackSmooth.reset(0.5f);
    mWowFlutterSmooth.reset(0.3f);
    mTapeAgeSmooth.reset(0.4f);
    mSatSmooth.reset(0.2f);
    mMixSmooth.reset(0.5f);
}

void TapeEchoEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;

    // Max delay 2.5s (to accommodate modulation)
    int maxSamples = static_cast<int>(sampleRate * 2.5f);
    mDelayL = DelayLine(maxSamples);
    mDelayR = DelayLine(maxSamples);

    mWowLfo.setSampleRate(sampleRate);
    mFlutterLfo.setSampleRate(sampleRate);

    // Configure smoothers
    mDelaySmooth.setSmoothingTime(50.0f, sampleRate);
    mFeedbackSmooth.setSmoothingTime(10.0f, sampleRate);
    mWowFlutterSmooth.setSmoothingTime(10.0f, sampleRate);
    mTapeAgeSmooth.setSmoothingTime(20.0f, sampleRate);
    mSatSmooth.setSmoothingTime(10.0f, sampleRate);
    mMixSmooth.setSmoothingTime(10.0f, sampleRate);

    // Initialize LPF
    mTapeLpfL.setLowpass(8000.0f, 0.707f);
    mTapeLpfR.setLowpass(8000.0f, 0.707f);
}

void TapeEchoEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_DELAY_TIME:
            mDelayTime.store(std::clamp(value, 50.0f, 2000.0f), std::memory_order_relaxed);
            break;
        case PARAM_FEEDBACK:
            mFeedback.store(std::clamp(value, 0.0f, 0.95f), std::memory_order_relaxed);
            break;
        case PARAM_WOW_FLUTTER:
            mWowFlutter.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_TAPE_AGE:
            mTapeAge.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_SATURATION:
            mSaturation.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
    }
}

float TapeEchoEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_DELAY_TIME: return mDelayTime.load(std::memory_order_relaxed);
        case PARAM_FEEDBACK: return mFeedback.load(std::memory_order_relaxed);
        case PARAM_WOW_FLUTTER: return mWowFlutter.load(std::memory_order_relaxed);
        case PARAM_TAPE_AGE: return mTapeAge.load(std::memory_order_relaxed);
        case PARAM_SATURATION: return mSaturation.load(std::memory_order_relaxed);
        case PARAM_MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void TapeEchoEffect::process(float* input, float* output, int numFrames) {
    // Load all params ONCE (atomic load)
    float delayMs = mDelayTime.load(std::memory_order_relaxed);
    float feedback = mFeedback.load(std::memory_order_relaxed);
    float wowFlutter = mWowFlutter.load(std::memory_order_relaxed);
    float tapeAge = mTapeAge.load(std::memory_order_relaxed);
    float saturation = mSaturation.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);

    // Tape Age controls LPF cutoff and hiss level (recalc per block)
    float smoothTapeAge = mTapeAgeSmooth.process(tapeAge);
    float lpfCutoff = 12000.0f * std::pow(1000.0f / 12000.0f, smoothTapeAge);
    mTapeLpfL.setLowpass(lpfCutoff, 0.707f);
    mTapeLpfR.setLowpass(lpfCutoff, 0.707f);
    float hissLevel = smoothTapeAge * 0.01f;

    for (int i = 0; i < numFrames; ++i) {
        float smoothDelay = mDelaySmooth.process(delayMs);
        float smoothFb = mFeedbackSmooth.process(feedback);
        float smoothWF = mWowFlutterSmooth.process(wowFlutter);
        float smoothSat = mSatSmooth.process(saturation);
        float smoothMix = mMixSmooth.process(mix);

        // Tape modulation (wow & flutter)
        float wow = mWowLfo.process() * smoothWF * 0.003f;
        float flutter = mFlutterLfo.process() * smoothWF * 0.0005f;
        float modMs = (wow + flutter) * smoothDelay;

        float modulatedDelay = smoothDelay + modMs;
        modulatedDelay = std::max(modulatedDelay, 1.0f);

        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        // Read from delay (cubic interpolation for smooth modulation)
        float delaySamples = modulatedDelay * static_cast<float>(mSampleRate) / 1000.0f;
        float delayedL = mDelayL.readInterpolated(delaySamples);
        float delayedR = mDelayR.readInterpolated(delaySamples);

        // Feedback path: LPF -> Saturation
        float fbL = mTapeLpfL.process(delayedL);
        float fbR = mTapeLpfR.process(delayedR);

        // Soft saturation (tape compression emulation)
        if (smoothSat > 0.001f) {
            float drive = 1.0f + smoothSat * 3.0f;
            fbL = std::tanh(fbL * drive) / drive;
            fbR = std::tanh(fbR * drive) / drive;
        }

        fbL *= smoothFb;
        fbR *= smoothFb;

        // Denormal protection on feedback path
        if (std::abs(fbL) < 1e-20f) fbL = 0.0f;
        if (std::abs(fbR) < 1e-20f) fbR = 0.0f;

        // Tape hiss
        float hiss = generateNoise() * hissLevel;

        // Write to delay
        mDelayL.write(dryL + fbL);
        mDelayR.write(dryR + fbR);

        // Output: dry + wet + hiss
        float wetL = delayedL + hiss;
        float wetR = delayedR + hiss;

        float outL = dryL + (wetL - dryL) * smoothMix;
        float outR = dryR + (wetR - dryR) * smoothMix;

        // NaN/Inf protection
        if (!std::isfinite(outL)) outL = dryL;
        if (!std::isfinite(outR)) outR = dryR;

        output[i * 2]     = outL;
        output[i * 2 + 1] = outR;
    }
}

float TapeEchoEffect::generateNoise() {
    mNoiseState ^= mNoiseState << 13;
    mNoiseState ^= mNoiseState >> 17;
    mNoiseState ^= mNoiseState << 5;
    return static_cast<float>(static_cast<int32_t>(mNoiseState)) / 2147483648.0f;
}
