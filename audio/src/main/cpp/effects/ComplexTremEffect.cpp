#include "ComplexTremEffect.h"
#include "../dsp/DSPMath.h"

ComplexTremEffect::ComplexTremEffect()
    : mLfo1L(48000.0f, 4.0f),
      mLfo2L(48000.0f, 5.5f),
      mLfo1R(48000.0f, 4.0f),
      mLfo2R(48000.0f, 5.5f) {
    mRate1Smooth.reset(4.0f);
    mRate2Smooth.reset(5.5f);
    mDepthSmooth.reset(0.6f);
    mMixSmooth.reset(1.0f);
}

void ComplexTremEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);

    mLfo1L.setSampleRate(sr);
    mLfo2L.setSampleRate(sr);
    mLfo1R.setSampleRate(sr);
    mLfo2R.setSampleRate(sr);

    mRate1Smooth.setSmoothingTime(10.0f, sr);
    mRate2Smooth.setSmoothingTime(10.0f, sr);
    mDepthSmooth.setSmoothingTime(10.0f, sr);
    mMixSmooth.setSmoothingTime(5.0f, sr);

    // Apply initial stereo phase offset to R channel
    float phaseRad = mStereoPhase.load(std::memory_order_relaxed) * DSPMath::PI / 180.0f;
    mLfo1R.setPhaseOffset(phaseRad);
    mLfo2R.setPhaseOffset(phaseRad);
    mLastStereoPhase = mStereoPhase.load(std::memory_order_relaxed);
}

void ComplexTremEffect::reset() {
    mLfo1L.reset();
    mLfo2L.reset();
    mLfo1R.reset();
    mLfo2R.reset();
    mLastStereoPhase = 0.0f;

    mRate1Smooth.reset(mRate1.load(std::memory_order_relaxed));
    mRate2Smooth.reset(mRate2.load(std::memory_order_relaxed));
    mDepthSmooth.reset(mDepth.load(std::memory_order_relaxed));
    mMixSmooth.reset(mMix.load(std::memory_order_relaxed));
}

void ComplexTremEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_RATE1:
            mRate1.store(std::clamp(value, 0.1f, 20.0f), std::memory_order_relaxed);
            break;
        case PARAM_RATE2:
            mRate2.store(std::clamp(value, 0.1f, 20.0f), std::memory_order_relaxed);
            break;
        case PARAM_DEPTH:
            mDepth.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case PARAM_WAVEFORM:
            mWaveform.store(std::clamp(static_cast<int>(value), 0, 3), std::memory_order_relaxed);
            break;
        case PARAM_STEREO_PHASE:
            mStereoPhase.store(std::clamp(value, 0.0f, 180.0f), std::memory_order_relaxed);
            break;
        case PARAM_MIX:
            mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
    }
}

float ComplexTremEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_RATE1:        return mRate1.load(std::memory_order_relaxed);
        case PARAM_RATE2:        return mRate2.load(std::memory_order_relaxed);
        case PARAM_DEPTH:        return mDepth.load(std::memory_order_relaxed);
        case PARAM_WAVEFORM:     return static_cast<float>(mWaveform.load(std::memory_order_relaxed));
        case PARAM_STEREO_PHASE: return mStereoPhase.load(std::memory_order_relaxed);
        case PARAM_MIX:          return mMix.load(std::memory_order_relaxed);
        default:                 return 0.0f;
    }
}

void ComplexTremEffect::process(float* input, float* output, int numFrames) {
    float rate1 = mRate1.load(std::memory_order_relaxed);
    float rate2 = mRate2.load(std::memory_order_relaxed);
    float depth = mDepth.load(std::memory_order_relaxed);
    int waveform = mWaveform.load(std::memory_order_relaxed);
    float stereoPhase = mStereoPhase.load(std::memory_order_relaxed);
    float mix = mMix.load(std::memory_order_relaxed);

    // Update LFO settings per-block
    mLfo1L.setRate(rate1);
    mLfo2L.setRate(rate2);
    mLfo1R.setRate(rate1);
    mLfo2R.setRate(rate2);

    static constexpr LFO::Waveform waveforms[] = {
        LFO::Waveform::SINE,
        LFO::Waveform::TRIANGLE,
        LFO::Waveform::SQUARE,
        LFO::Waveform::SAWTOOTH
    };
    int safeWaveform = std::clamp(waveform, 0, 3);
    LFO::Waveform wf = waveforms[safeWaveform];
    mLfo1L.setWaveform(wf);
    mLfo2L.setWaveform(wf);
    mLfo1R.setWaveform(wf);
    mLfo2R.setWaveform(wf);

    // Update stereo phase offset if changed
    if (std::abs(stereoPhase - mLastStereoPhase) > 0.1f) {
        float phaseRad = stereoPhase * DSPMath::PI / 180.0f;
        mLfo1R.setPhaseOffset(phaseRad);
        mLfo2R.setPhaseOffset(phaseRad);
        mLastStereoPhase = stereoPhase;
    }

    for (int i = 0; i < numFrames; ++i) {
        float smoothDepth = mDepthSmooth.process(depth);
        float smoothMix = mMixSmooth.process(mix);

        // Left channel: multiplicative combination of two LFOs
        float lfo1L = mLfo1L.processUnipolar();  // [0, 1]
        float lfo2L = mLfo2L.processUnipolar();  // [0, 1]
        float modulationL = lfo1L * lfo2L;
        float gainL = 1.0f - smoothDepth * (1.0f - modulationL);

        // Right channel: same but with stereo phase offset
        float lfo1R = mLfo1R.processUnipolar();
        float lfo2R = mLfo2R.processUnipolar();
        float modulationR = lfo1R * lfo2R;
        float gainR = 1.0f - smoothDepth * (1.0f - modulationR);

        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];

        float wetL = dryL * gainL;
        float wetR = dryR * gainR;

        output[i * 2]     = dryL + (wetL - dryL) * smoothMix;
        output[i * 2 + 1] = dryR + (wetR - dryR) * smoothMix;
    }
}
