#pragma once

#include "Effect.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/FDN.h"
#include "../dsp/GrainEngine.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>

class ShimmerReverbEffect : public Effect {
public:
    static constexpr int PARAM_DECAY = 0;
    static constexpr int PARAM_SIZE = 1;
    static constexpr int PARAM_PITCH_SEMITONES = 2;
    static constexpr int PARAM_SHIMMER_AMOUNT = 3;
    static constexpr int PARAM_FEEDBACK = 4;
    static constexpr int PARAM_TONE = 5;
    static constexpr int PARAM_MIX = 6;
    static constexpr int PARAM_COUNT = 7;

    ShimmerReverbEffect();

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;
    void reset() override;

private:
    FDN mFdn;
    GrainEngine mPitchGrains;
    BiquadFilter mToneL;
    BiquadFilter mToneR;

    std::atomic<float> mDecay{5.0f};
    std::atomic<float> mSize{0.85f};
    std::atomic<float> mPitchSemitones{12.0f};
    std::atomic<float> mShimmerAmount{0.35f};
    std::atomic<float> mFeedback{0.35f};
    std::atomic<float> mTone{0.65f};
    std::atomic<float> mMix{0.35f};

    ParameterSmoother mMixSmooth;
    ParameterSmoother mShimmerSmooth;
    int mSampleRate = 48000;
    int mGrainCounter = 0;
    float mFeedbackL = 0.0f;
    float mFeedbackR = 0.0f;
    float mLastTone = 0.65f;
};
