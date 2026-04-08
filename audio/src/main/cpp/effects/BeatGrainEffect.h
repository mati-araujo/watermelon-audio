#pragma once

#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include "../dsp/GrainEngine.h"
#include <atomic>

/**
 * @file BeatGrainEffect.h
 * @brief Beat-synced granular delay effect (KORG NTS-3 FX-003).
 *
 * Fragments the input signal into grains dispatched rhythmically
 * in sync with the global BPM. Uses GrainEngine for voice management.
 *
 * Parameters:
 * - 0: Grain Size (1-200 ms)
 * - 1: Density (0-3: 1/4, 1/8, 1/16, 1/32)
 * - 2: Position Spread (0-1)
 * - 3: Pitch Shift (-12 to +12 semitones)
 * - 4: Buffer Length (0.5-4 s) — logical, buffer is always pre-allocated to 4s
 * - 5: Mix (0-1)
 *
 * XY mapping: X = Grain Size, Y = Density, Depth = Mix
 */
class BeatGrainEffect : public Effect {
public:
    static constexpr int PARAM_GRAIN_SIZE = 0;
    static constexpr int PARAM_DENSITY = 1;
    static constexpr int PARAM_POSITION_SPREAD = 2;
    static constexpr int PARAM_PITCH_SHIFT = 3;
    static constexpr int PARAM_BUFFER_LENGTH = 4;
    static constexpr int PARAM_MIX = 5;
    static constexpr int PARAM_COUNT = 6;

    BeatGrainEffect();
    ~BeatGrainEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;
    void setBpm(float bpm) override;

private:
    GrainEngine mGrainEngine;

    std::atomic<float> mGrainSize{50.0f};
    std::atomic<int> mDensity{2};
    std::atomic<float> mPositionSpread{0.1f};
    std::atomic<float> mPitchShift{0.0f};
    std::atomic<float> mBufferLength{2.0f};
    std::atomic<float> mMix{0.5f};
    std::atomic<float> mBpm{120.0f};

    float mSchedulerCounter = 0.0f;

    ParameterSmoother mMixSmooth;
    int mSampleRate = 48000;

    float getGrainIntervalSamples(int density, float bpm);
};
