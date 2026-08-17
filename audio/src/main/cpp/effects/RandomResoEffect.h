#pragma once
#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/LFO.h"
#include <atomic>
#include <cmath>
#include <algorithm>

/**
 * @class RandomResoEffect
 * @brief LFO-modulated resonant bandpass filter (KORG NTS-3 FX-001)
 *
 * Resonant BPF whose center frequency is modulated by an LFO set to
 * RANDOM_SMOOTH waveform. Produces unpredictable tonal sweeps — a random auto-wah.
 * The LFO modulates frequency in logarithmic (octave) scale.
 *
 * Sub-block processing recalculates filter coefficients every 32 samples
 * for smooth modulation up to 20 Hz without per-sample coefficient updates.
 */
class RandomResoEffect : public Effect {
public:
    RandomResoEffect();
    ~RandomResoEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    /// Limpia los filtros y el LFO de random, y re-siembra el mix (WD-3.2).
    void reset() override;

    // Parameter IDs
    static constexpr int PARAM_CENTER_FREQ = 0;   // 80-12000 Hz (XY: X)
    static constexpr int PARAM_RESONANCE = 1;      // 0.5-30 (XY: Y)
    static constexpr int PARAM_LFO_RATE = 2;       // 0.1-20 Hz (XY: Depth)
    static constexpr int PARAM_LFO_DEPTH = 3;      // 0-1 (mapped to 0-4 octaves)
    static constexpr int PARAM_MIX = 4;            // 0-1

private:
    BiquadFilter mFilterL;
    BiquadFilter mFilterR;
    LFO mRandomLfo;

    std::atomic<float> mCenterFreq{1000.0f};
    std::atomic<float> mResonance{8.0f};
    std::atomic<float> mLfoRate{2.0f};
    std::atomic<float> mLfoDepth{0.5f};
    std::atomic<float> mMix{1.0f};

    ParameterSmoother mMixSmooth;

    int mSampleRate = 48000;

    static constexpr int SUB_BLOCK = 32;
};
