#pragma once
#include "Effect.h"
#include "../dsp/DelayLine.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>

class RiserReverbEffect : public Effect {
public:
    RiserReverbEffect();
    ~RiserReverbEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    static constexpr int PARAM_ATTACK_TIME = 0;
    static constexpr int PARAM_DECAY = 1;
    static constexpr int PARAM_SIZE = 2;
    static constexpr int PARAM_DIFFUSION = 3;
    static constexpr int PARAM_DAMPING = 4;
    static constexpr int PARAM_MIX = 5;

private:
    static constexpr int NUM_TAPS = 12;

    // Pre-delay
    DelayLine mPreDelayL;
    DelayLine mPreDelayR;

    // Multitap delay with rising envelope
    DelayLine mTapDelayL;
    DelayLine mTapDelayR;

    // Diffusion filters (2 cascaded LPFs per channel for smearing)
    BiquadFilter mDiffuseL1;
    BiquadFilter mDiffuseL2;
    BiquadFilter mDiffuseR1;
    BiquadFilter mDiffuseR2;

    // LPF for darkening
    BiquadFilter mTapLpfL;
    BiquadFilter mTapLpfR;

    // Parameters
    std::atomic<float> mAttackTime{800.0f};
    std::atomic<float> mDecay{2.0f};
    std::atomic<float> mSize{0.6f};
    std::atomic<float> mDiffusion{0.7f};
    std::atomic<float> mDamping{0.4f};
    std::atomic<float> mMix{0.5f};

    // Smoothers
    ParameterSmoother mAttackSmooth;
    ParameterSmoother mSizeSmooth;
    ParameterSmoother mMixSmooth;
    ParameterSmoother mDampSmooth;

    int mSampleRate = 48000;

    static constexpr float TAP_BASE_MS[NUM_TAPS] = {
        15.0f, 35.0f, 60.0f, 90.0f, 130.0f, 180.0f,
        240.0f, 320.0f, 420.0f, 550.0f, 700.0f, 900.0f
    };

    void setupAllpass(float diffusion);
};
