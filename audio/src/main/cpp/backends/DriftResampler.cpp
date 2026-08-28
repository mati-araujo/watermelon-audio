#include "DriftResampler.h"

#include <algorithm>
#include <cmath>

namespace watermelon_audio {

DriftResampler::DriftResampler(float sourceRateHz, float targetRateHz)
    : mSourceRate(sourceRateHz)
    , mTargetRate(targetRateHz) {
    updateStep();
}

void DriftResampler::configure(float sourceRateHz, float targetRateHz) {
    mSourceRate = sourceRateHz > 0.0f ? sourceRateHz : 48000.0f;
    mTargetRate = targetRateHz > 0.0f ? targetRateHz : mSourceRate;
    updateStep();
    reset();
}

void DriftResampler::reset() {
    mFractionalPos = 0.0;
    mHistory.fill(0.0f);
}

int DriftResampler::process(
    const float* input,
    int inFrames,
    int channels,
    float* output,
    int outCapacityFrames) {

    if (!input || !output || inFrames <= 0 || outCapacityFrames <= 0 || channels <= 0) {
        return 0;
    }

    channels = std::min(channels, kMaxChannels);

    int outFrames = 0;
    double pos = mFractionalPos;

    while (outFrames < outCapacityFrames && pos < static_cast<double>(inFrames)) {
        const int index = static_cast<int>(pos);
        const double frac = pos - static_cast<double>(index);
        const int next = (index + 1 < inFrames) ? index + 1 : index;

        for (int ch = 0; ch < channels; ++ch) {
            const float a = input[index * channels + ch];
            const float b = input[next * channels + ch];
            output[outFrames * channels + ch] =
                a + static_cast<float>(frac) * (b - a);
        }

        ++outFrames;
        pos += static_cast<double>(mStep);
    }

    const int lastFrame = inFrames - 1;
    for (int ch = 0; ch < channels; ++ch) {
        mHistory[ch] = input[lastFrame * channels + ch];
    }

    mFractionalPos = pos - static_cast<double>(inFrames);
    if (mFractionalPos < 0.0 || !std::isfinite(mFractionalPos)) {
        mFractionalPos = 0.0;
    }

    return outFrames;
}

void DriftResampler::updateStep() {
    const float safeSource = mSourceRate > 0.0f ? mSourceRate : 48000.0f;
    const float safeTarget = mTargetRate > 0.0f ? mTargetRate : safeSource;
    mStep = std::clamp(safeSource / safeTarget, 0.25f, 4.0f);
}

} // namespace watermelon_audio
