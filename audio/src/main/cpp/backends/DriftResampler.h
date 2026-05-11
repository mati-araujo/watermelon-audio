/**
 * DriftResampler.h
 *
 * Small linear resampler for SplitBackend clock reconciliation.
 */

#pragma once

#include <array>
#include <cstdint>

namespace watermelon_audio {

class DriftResampler {
public:
    DriftResampler(float sourceRateHz = 48000.0f, float targetRateHz = 48000.0f);

    void configure(float sourceRateHz, float targetRateHz);
    void setDriftCorrection(float ppm);
    void reset();

    int process(
        const float* input,
        int inFrames,
        int channels,
        float* output,
        int outCapacityFrames);

    float ratio() const { return mStep; }

private:
    static constexpr int kMaxChannels = 8;

    float mSourceRate = 48000.0f;
    float mTargetRate = 48000.0f;
    float mDriftPpm = 0.0f;
    float mStep = 1.0f;
    double mFractionalPos = 0.0;
    std::array<float, kMaxChannels> mHistory{};

    void updateStep();
};

} // namespace watermelon_audio
