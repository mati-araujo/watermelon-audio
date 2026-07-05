#pragma once

#include "Effect.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/DelayLine.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>

class SpringReverbEffect : public Effect {
public:
    static constexpr int PARAM_DECAY = 0;
    static constexpr int PARAM_TONE = 1;
    static constexpr int PARAM_DRIP = 2;
    static constexpr int PARAM_TENSION = 3;
    static constexpr int PARAM_MIX = 4;
    static constexpr int PARAM_COUNT = 5;

    SpringReverbEffect();

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;
    void reset() override;

private:
    DelayLine mTankL;
    DelayLine mTankR;
    BiquadFilter mToneL;
    BiquadFilter mToneR;
    BiquadFilter mInputHpfL;
    BiquadFilter mInputHpfR;

    std::atomic<float> mDecay{2.2f};
    std::atomic<float> mTone{0.55f};
    std::atomic<float> mDrip{0.35f};
    std::atomic<float> mTension{0.5f};
    std::atomic<float> mMix{0.25f};

    ParameterSmoother mMixSmooth;
    ParameterSmoother mDecaySmooth;
    int mSampleRate = 48000;
    float mLastTone = 0.55f;
};
