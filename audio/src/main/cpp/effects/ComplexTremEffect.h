#pragma once
#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include "../dsp/LFO.h"
#include <atomic>
#include <cmath>
#include <algorithm>

/**
 * @class ComplexTremEffect
 * @brief Dual-LFO multiplicative tremolo (KORG NTS-3 FX-007)
 *
 * Complex tremolo using two LFOs combined by multiplication.
 * The interaction of different frequencies produces complex polyrhythmic
 * volume patterns. Supports stereo phase offset for spatial movement.
 */
class ComplexTremEffect : public Effect {
public:
    ComplexTremEffect();
    ~ComplexTremEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    // Parameter IDs
    static constexpr int PARAM_RATE1 = 0;          // 0.1-20 Hz (XY: X)
    static constexpr int PARAM_RATE2 = 1;          // 0.1-20 Hz (XY: Y)
    static constexpr int PARAM_DEPTH = 2;          // 0-1 (XY: Depth)
    static constexpr int PARAM_WAVEFORM = 3;       // 0-3 (Sine/Tri/Sq/Saw)
    static constexpr int PARAM_STEREO_PHASE = 4;   // 0-180 degrees
    static constexpr int PARAM_MIX = 5;            // 0-1

private:
    // LFOs for left channel
    LFO mLfo1L;
    LFO mLfo2L;
    // LFOs for right channel (with phase offset for stereo)
    LFO mLfo1R;
    LFO mLfo2R;

    // Parameters
    std::atomic<float> mRate1{4.0f};
    std::atomic<float> mRate2{5.5f};
    std::atomic<float> mDepth{0.6f};
    std::atomic<int> mWaveform{0};
    std::atomic<float> mStereoPhase{0.0f};
    std::atomic<float> mMix{1.0f};

    // Smoothers
    ParameterSmoother mRate1Smooth;
    ParameterSmoother mRate2Smooth;
    ParameterSmoother mDepthSmooth;
    ParameterSmoother mMixSmooth;

    int mSampleRate = 48000;
    float mLastStereoPhase = 0.0f;
};
