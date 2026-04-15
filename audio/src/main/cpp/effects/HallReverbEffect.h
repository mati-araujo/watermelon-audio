#pragma once
#include "Effect.h"
#include "../dsp/DelayLine.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/FDN.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>

class HallReverbEffect : public Effect {
public:
    HallReverbEffect();
    ~HallReverbEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    /**
     * @brief Clear pre-delay, early reflection, and FDN late-reverb state.
     * RT-safe: delegates to sub-component clear/reset methods, no allocation.
     */
    void reset() override;

    static constexpr int PARAM_DECAY_TIME = 0;
    static constexpr int PARAM_SIZE = 1;
    static constexpr int PARAM_PRE_DELAY = 2;
    static constexpr int PARAM_DIFFUSION = 3;
    static constexpr int PARAM_HF_DAMPING = 4;
    static constexpr int PARAM_LF_DAMPING = 5;
    static constexpr int PARAM_MODULATION = 6;
    static constexpr int PARAM_MIX = 7;

private:
    // Pre-delay
    DelayLine mPreDelayL;
    DelayLine mPreDelayR;

    // Early reflections: multitap delay
    DelayLine mEarlyL;
    DelayLine mEarlyR;

    // Late reverb: FDN
    FDN mFdn;

    // Parameters
    std::atomic<float> mDecayTime{3.0f};
    std::atomic<float> mSize{0.7f};
    std::atomic<float> mPreDelay{30.0f};
    std::atomic<float> mDiffusion{0.8f};
    std::atomic<float> mHfDamping{0.4f};
    std::atomic<float> mLfDamping{0.2f};
    std::atomic<float> mModulation{0.15f};
    std::atomic<float> mMix{0.3f};

    // Smoothers
    ParameterSmoother mDecaySmooth;
    ParameterSmoother mSizeSmooth;
    ParameterSmoother mPreDelaySmooth;
    ParameterSmoother mDiffusionSmooth;
    ParameterSmoother mMixSmooth;

    int mSampleRate = 48000;

    static constexpr int NUM_EARLY_TAPS = 6;
    static constexpr float EARLY_DELAYS_MS[NUM_EARLY_TAPS] = {
        5.3f, 11.7f, 19.1f, 27.3f, 36.7f, 48.9f
    };
    static constexpr float EARLY_GAINS[NUM_EARLY_TAPS] = {
        0.85f, 0.72f, 0.60f, 0.50f, 0.38f, 0.28f
    };
};
