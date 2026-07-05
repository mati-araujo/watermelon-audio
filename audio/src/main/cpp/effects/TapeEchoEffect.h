#pragma once
#include "Effect.h"
#include "../dsp/DelayLine.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/LFO.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <cstdint>

class TapeEchoEffect : public Effect {
public:
    TapeEchoEffect();
    ~TapeEchoEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;
    void setBpm(float bpm) override;

    /**
     * @brief Clear delay lines, feedback LPF state, and wow/flutter LFOs.
     */
    void reset() override;

    static constexpr int PARAM_DELAY_TIME = 0;
    static constexpr int PARAM_FEEDBACK = 1;
    static constexpr int PARAM_WOW_FLUTTER = 2;
    static constexpr int PARAM_TAPE_AGE = 3;
    static constexpr int PARAM_SATURATION = 4;
    static constexpr int PARAM_MIX = 5;
    static constexpr int PARAM_SYNC = 6;
    static constexpr int PARAM_SUBDIVISION = 7;
    static constexpr int PARAM_PING_PONG = 8;
    static constexpr int PARAM_DUCKING = 9;
    static constexpr int PARAM_NOISE_LEVEL = 10;
    static constexpr int PARAM_COUNT = 11;

private:
    float generateNoise();

    // Delay lines
    DelayLine mDelayL;
    DelayLine mDelayR;

    // LPF in feedback path (tape darkening)
    BiquadFilter mTapeLpfL;
    BiquadFilter mTapeLpfR;

    // Wow: slow sine LFO (0.5-3 Hz)
    LFO mWowLfo;
    // Flutter: fast random-smooth LFO (3-10 Hz)
    LFO mFlutterLfo;

    // Parameters (atomic for RT-safe access)
    std::atomic<float> mDelayTime{350.0f};
    std::atomic<float> mFeedback{0.5f};
    std::atomic<float> mWowFlutter{0.3f};
    std::atomic<float> mTapeAge{0.4f};
    std::atomic<float> mSaturation{0.2f};
    std::atomic<float> mMix{0.5f};
    std::atomic<float> mBpm{120.0f};
    std::atomic<bool> mSync{false};
    std::atomic<int> mSubdivision{2};
    std::atomic<bool> mPingPong{false};
    std::atomic<float> mDucking{0.0f};
    std::atomic<float> mNoiseLevel{0.0f};

    // Parameter smoothers
    ParameterSmoother mDelaySmooth;
    ParameterSmoother mFeedbackSmooth;
    ParameterSmoother mWowFlutterSmooth;
    ParameterSmoother mTapeAgeSmooth;
    ParameterSmoother mSatSmooth;
    ParameterSmoother mMixSmooth;

    // Hiss generator (xorshift PRNG)
    uint32_t mNoiseState = 12345;

    int mSampleRate = 48000;
};
