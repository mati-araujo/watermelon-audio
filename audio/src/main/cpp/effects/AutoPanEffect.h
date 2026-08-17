#pragma once
#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include "../dsp/LFO.h"
#include "../dsp/DSPMath.h"
#include <atomic>
#include <cmath>
#include <algorithm>

/**
 * @class AutoPanEffect
 * @brief LFO-driven stereo auto-panner (KORG NTS-3 FX-008)
 *
 * Automatically pans audio between left and right channels using an LFO.
 * Uses equal-power panning (cosine law) to maintain perceived loudness.
 * Sums input to mono then applies pan modulation for clean stereo movement.
 */
class AutoPanEffect : public Effect {
public:
    AutoPanEffect();
    ~AutoPanEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    /// Limpia la fase del LFO y re-siembra los smoothers (WD-3.2).
    void reset() override;

    // Parameter IDs
    static constexpr int PARAM_RATE = 0;           // 0.1-20 Hz (XY: X)
    static constexpr int PARAM_DEPTH = 1;          // 0-1 (XY: Y)
    static constexpr int PARAM_WAVEFORM = 2;       // 0-2 (Sine/Tri/Sq)
    static constexpr int PARAM_PHASE_OFFSET = 3;   // 0-360 degrees
    static constexpr int PARAM_MIX = 4;            // 0-1 (XY: Depth)

private:
    LFO mLfo;

    // Parameters
    std::atomic<float> mRate{2.0f};
    std::atomic<float> mDepth{0.8f};
    std::atomic<int> mWaveform{0};
    std::atomic<float> mPhaseOffset{0.0f};
    std::atomic<float> mMix{1.0f};

    // Smoothers
    ParameterSmoother mRateSmooth;
    ParameterSmoother mDepthSmooth;
    ParameterSmoother mMixSmooth;

    int mSampleRate = 48000;
};
