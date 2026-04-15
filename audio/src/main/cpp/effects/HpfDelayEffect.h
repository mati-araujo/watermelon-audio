#pragma once
#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/DelayLine.h"
#include <atomic>
#include <cmath>
#include <algorithm>

/**
 * @class HpfDelayEffect
 * @brief High-pass filtered delay (KORG NTS-3 FX-004)
 *
 * Delay with a HPF inside the feedback loop. Each repetition loses more
 * low-frequency content, producing echoes that progressively thin out —
 * airy and bright.
 *
 * The HPF naturally reduces energy accumulation in the feedback path,
 * making it more stable at high feedback values.
 */
class HpfDelayEffect : public Effect {
public:
    HpfDelayEffect();
    ~HpfDelayEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    /**
     * @brief Clear delay lines and feedback HPF state. RT-safe.
     */
    void reset() override;

    // Parameter IDs
    static constexpr int PARAM_HPF_CUTOFF = 0;    // 20-8000 Hz (XY: X)
    static constexpr int PARAM_DELAY_TIME = 1;     // 10-2000 ms (XY: Y)
    static constexpr int PARAM_FEEDBACK = 2;       // 0-0.95 (XY: Depth)
    static constexpr int PARAM_MIX = 3;            // 0-1

private:
    DelayLine mDelayL;
    DelayLine mDelayR;
    BiquadFilter mHpfL;
    BiquadFilter mHpfR;

    std::atomic<float> mHpfCutoff{200.0f};
    std::atomic<float> mDelayTime{300.0f};
    std::atomic<float> mFeedback{0.4f};
    std::atomic<float> mMix{0.5f};

    ParameterSmoother mDelaySmooth;
    ParameterSmoother mFeedbackSmooth;
    ParameterSmoother mMixSmooth;

    int mSampleRate = 48000;

    float mLastHpfCutoff = 200.0f;
};
