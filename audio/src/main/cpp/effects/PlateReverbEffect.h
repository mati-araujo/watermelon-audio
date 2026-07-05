#pragma once

#include "Effect.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/DelayLine.h"
#include "../dsp/FDN.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>

class PlateReverbEffect : public Effect {
public:
    static constexpr int PARAM_DECAY = 0;
    static constexpr int PARAM_PRE_DELAY = 1;
    static constexpr int PARAM_DAMPING = 2;
    static constexpr int PARAM_MODULATION = 3;
    static constexpr int PARAM_LOW_CUT = 4;
    static constexpr int PARAM_HIGH_CUT = 5;
    static constexpr int PARAM_MIX = 6;
    static constexpr int PARAM_COUNT = 7;

    PlateReverbEffect();

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;
    void reset() override;

private:
    DelayLine mPreDelayL;
    DelayLine mPreDelayR;
    FDN mFdn;
    BiquadFilter mLowCutL;
    BiquadFilter mLowCutR;
    BiquadFilter mHighCutL;
    BiquadFilter mHighCutR;

    std::atomic<float> mDecay{2.4f};
    std::atomic<float> mPreDelay{18.0f};
    std::atomic<float> mDamping{0.35f};
    std::atomic<float> mModulation{0.12f};
    std::atomic<float> mLowCut{120.0f};
    std::atomic<float> mHighCut{9000.0f};
    std::atomic<float> mMix{0.28f};

    ParameterSmoother mMixSmooth;
    ParameterSmoother mPreDelaySmooth;
    int mSampleRate = 48000;
    float mLastLowCut = 120.0f;
    float mLastHighCut = 9000.0f;
};
