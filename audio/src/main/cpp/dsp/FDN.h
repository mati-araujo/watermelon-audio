#pragma once

#include "DelayLine.h"
#include "BiquadFilter.h"
#include "LFO.h"
#include <atomic>
#include <cmath>

/**
 * @brief 8-channel Feedback Delay Network for dense reverb tails.
 *
 * Uses Hadamard mixing matrix, per-channel damping (LPF + HPF),
 * and slow delay modulation to avoid metallic coloration.
 *
 * Thread-safety: All parameters are atomic. Filter and gain updates
 * use snapshot patterns to avoid partial reads from the audio thread.
 */
class FDN {
public:
    static constexpr int FDN_CHANNELS = 8;

    FDN();
    ~FDN() = default;

    void setSampleRate(int sampleRate);
    void setDecayTime(float seconds);
    void setSize(float size);
    void setDamping(float hfDamping, float lfDamping);
    void setModulation(float depth);

    void process(float inputL, float inputR, float& outputL, float& outputR);
    void reset();

private:
    DelayLine mDelays[FDN_CHANNELS];

    BiquadFilter mLpf[FDN_CHANNELS];
    BiquadFilter mHpf[FDN_CHANNELS];

    LFO mModLfo[FDN_CHANNELS];

    // Parameters — all atomic for RT-safe access from UI thread
    std::atomic<float> mDecayTime{3.0f};
    std::atomic<float> mSize{0.7f};
    std::atomic<float> mModDepth{0.15f};
    std::atomic<float> mHfDamping{0.4f};
    std::atomic<float> mLfDamping{0.2f};
    std::atomic<int> mSampleRate{48000};

    static constexpr float BASE_DELAYS_MS[FDN_CHANNELS] = {
        29.7f, 37.1f, 44.3f, 53.9f, 61.7f, 73.1f, 83.3f, 97.1f
    };

    // Feedback gains — double-buffered for glitch-free updates
    float mFeedbackGainA[FDN_CHANNELS] = {};
    float mFeedbackGainB[FDN_CHANNELS] = {};
    std::atomic<int> mActiveGainBuffer{0};  // 0 = A, 1 = B

    // Cached smoothed values for audio thread
    float mSmoothSize = 0.7f;
    float mSmoothDecayTime = 3.0f;

    static constexpr float DENORMAL_THRESHOLD = 1e-20f;

    void updateFeedbackGains();
    void updateDamping();
    void applyHadamardMix(float* state);
};
