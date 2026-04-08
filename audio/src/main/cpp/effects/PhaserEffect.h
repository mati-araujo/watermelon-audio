#ifndef PHASEREFFECT_H
#define PHASEREFFECT_H

#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <array>
#include <cmath>

/**
 * @file PhaserEffect.h
 * @brief All-pass phaser with variable stages
 *
 * Emulates classic phaser pedals like MXR Phase 90 and EHX Small Stone.
 *
 * Features:
 * - Rate: LFO speed (0.01 to 10 Hz)
 * - Depth: Modulation amount (0 to 100%)
 * - Stages: Number of all-pass stages (2, 4, 6, 8, 12)
 * - Feedback: Signal feedback for more resonance (-90 to +90%)
 * - Mix: Wet/dry balance (0 to 100%)
 *
 * Use cases:
 * - Classic guitar "swooshing" effect
 * - Jet sounds (high feedback)
 * - Subtle movement (low rate, low depth)
 *
 * Thread-safe: All parameters use atomic operations.
 */
class PhaserEffect : public Effect {
public:
    /**
     * @brief Parameter IDs
     */
    enum Param {
        RATE = 0,       ///< 0.01 to 10 Hz
        DEPTH = 1,      ///< 0 to 100 %
        STAGES = 2,     ///< 2, 4, 6, 8, 12
        FEEDBACK = 3,   ///< -90 to +90 %
        MIX = 4,        ///< 0 to 100 %
        PARAM_COUNT = 5
    };

    PhaserEffect();
    ~PhaserEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

private:
    static constexpr int MAX_STAGES = 12;
    static constexpr float MIN_FREQ = 200.0f;
    static constexpr float MAX_FREQ = 5000.0f;

    int mSampleRate{48000};

    // Parameters
    std::atomic<float> mRate{0.5f};      // Hz
    std::atomic<float> mDepth{70.0f};    // %
    std::atomic<int> mStages{4};
    std::atomic<float> mFeedback{30.0f}; // %
    std::atomic<float> mMix{50.0f};      // %

    // All-pass filter states (stereo)
    std::array<float, MAX_STAGES> mAllPassStateL{};
    std::array<float, MAX_STAGES> mAllPassStateR{};

    // Previous input for all-pass filter
    std::array<float, MAX_STAGES> mPrevInputL{};
    std::array<float, MAX_STAGES> mPrevInputR{};

    // LFO
    float mLfoPhase = 0.0f;

    // Feedback
    float mFeedbackL = 0.0f;
    float mFeedbackR = 0.0f;

    // Parameter smoothers (prevent clicks)
    ParameterSmoother mMixSmoother{0.995f};
    ParameterSmoother mFeedbackSmoother{0.995f};

    // Process single all-pass stage
    float processAllPass(float input, float& prevInput, float& state, float coefficient);
    void reset();
};

#endif // PHASEREFFECT_H
