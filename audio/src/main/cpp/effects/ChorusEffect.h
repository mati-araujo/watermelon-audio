#ifndef CHORUSEFFECT_H
#define CHORUSEFFECT_H

#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <array>
#include <cmath>

/**
 * @file ChorusEffect.h
 * @brief Multi-voice chorus effect with LFO modulation
 *
 * Emulates classic chorus pedals like Boss CE-2 and Roland Dimension D.
 *
 * Features:
 * - Rate: LFO speed (0.1 to 10 Hz)
 * - Depth: Modulation amount (0 to 100%)
 * - Delay: Base delay time (1 to 30 ms)
 * - Feedback: Delay line feedback (-50 to +50%)
 * - Mix: Wet/dry balance (0 to 100%)
 * - Voices: Number of chorus voices (1 to 4)
 *
 * Use cases:
 * - Classic guitar "lush" sound
 * - Stereo widening
 * - Vibrato effect (100% wet, single voice)
 *
 * Thread-safe: All parameters use atomic operations.
 */
class ChorusEffect : public Effect {
public:
    /**
     * @brief Parameter IDs
     */
    enum Param {
        RATE = 0,       ///< 0.1 to 10 Hz
        DEPTH = 1,      ///< 0 to 100 %
        DELAY = 2,      ///< 1 to 30 ms
        FEEDBACK = 3,   ///< -50 to +50 %
        MIX = 4,        ///< 0 to 100 %
        VOICES = 5,     ///< 1 to 4
        PARAM_COUNT = 6
    };

    ChorusEffect();
    ~ChorusEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    /**
     * @brief Clear delay lines and LFO phase (RT-safe, no allocation).
     */
    void reset() override;

private:
    static constexpr int MAX_DELAY_SAMPLES = 4800;  // 100ms at 48kHz
    static constexpr int MAX_VOICES = 4;

    int mSampleRate{48000};

    // Parameters
    std::atomic<float> mRate{1.0f};         // Hz
    std::atomic<float> mDepth{50.0f};       // %
    std::atomic<float> mDelayMs{7.0f};      // ms
    std::atomic<float> mFeedback{0.0f};     // %
    std::atomic<float> mMix{50.0f};         // %
    std::atomic<int> mVoices{2};

    // Delay lines (stereo)
    std::array<float, MAX_DELAY_SAMPLES> mDelayLineL{};
    std::array<float, MAX_DELAY_SAMPLES> mDelayLineR{};
    int mWriteIndex = 0;

    // LFO state (per voice)
    std::array<float, MAX_VOICES> mLfoPhase{};

    // Parameter smoother for mix (prevents clicks)
    ParameterSmoother mMixSmoother{0.995f};

    float interpolatedRead(const std::array<float, MAX_DELAY_SAMPLES>& line, float delaySamples) const;
};

#endif // CHORUSEFFECT_H
