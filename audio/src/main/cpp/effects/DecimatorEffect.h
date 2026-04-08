#pragma once
#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <cmath>
#include <algorithm>

/**
 * @class DecimatorEffect
 * @brief Bit crusher + sample rate reducer (KORG NTS-3 FX-002)
 *
 * Reduces bit depth and sample rate of the audio signal to create
 * lo-fi digital artifacts. Uses zero-order hold for sample rate
 * reduction and quantization for bit depth reduction.
 *
 * Parameters are smoothed to prevent zipper noise during adjustment.
 */
class DecimatorEffect : public Effect {
public:
    DecimatorEffect();
    ~DecimatorEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    // Parameter IDs
    static constexpr int PARAM_BIT_DEPTH = 0;     // 1-24 bits
    static constexpr int PARAM_SAMPLE_RATE = 1;    // 100-48000 Hz
    static constexpr int PARAM_MIX = 2;            // 0-1

private:
    // Parameters (atomic for thread safety)
    std::atomic<float> mBitDepth{16.0f};
    std::atomic<float> mTargetSampleRate{48000.0f};
    std::atomic<float> mMix{1.0f};

    // State
    float mHoldL = 0.0f;        // Zero-order hold: last retained sample L
    float mHoldR = 0.0f;        // Zero-order hold: last retained sample R
    float mHoldCounter = 0.0f;  // Fractional counter for decimation
    int mSampleRate = 48000;

    // Smoothers to prevent zipper noise
    ParameterSmoother mBitDepthSmooth;
    ParameterSmoother mSampleRateSmooth;
    ParameterSmoother mMixSmooth;
};
