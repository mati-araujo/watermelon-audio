#include "../../backends/DriftResampler.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace watermelon_audio;

namespace {

TEST(DriftResamplerTest, UnityRatioPreservesFrameCountAndSamples) {
    DriftResampler resampler(48000.0f, 48000.0f);

    std::vector<float> input = {
        0.0f, 0.1f,
        1.0f, 1.1f,
        2.0f, 2.1f,
        3.0f, 3.1f
    };
    std::vector<float> output(8, -1.0f);

    const int frames = resampler.process(input.data(), 4, 2, output.data(), 4);

    ASSERT_EQ(frames, 4);
    for (size_t i = 0; i < input.size(); ++i) {
        EXPECT_FLOAT_EQ(output[i], input[i]);
    }
}

TEST(DriftResamplerTest, PositivePpmProducesSlightlyFewerFramesOverLongWindow) {
    DriftResampler resampler(48000.0f, 48000.0f);
    resampler.setDriftCorrection(500.0f);

    constexpr int kFrames = 48000;
    std::vector<float> input(static_cast<size_t>(kFrames * 2), 0.0f);
    std::vector<float> output(static_cast<size_t>(kFrames * 2), 0.0f);

    for (int frame = 0; frame < kFrames; ++frame) {
        input[frame * 2] = static_cast<float>(frame);
        input[frame * 2 + 1] = static_cast<float>(frame);
    }

    const int frames = resampler.process(input.data(), kFrames, 2, output.data(), kFrames);

    EXPECT_LT(frames, kFrames);
    EXPECT_NEAR(frames, 47976, 2);
}

TEST(DriftResamplerTest, NegativePpmProducesSlightlyMoreFramesWhenCapacityAllows) {
    DriftResampler resampler(48000.0f, 48000.0f);
    resampler.setDriftCorrection(-500.0f);

    constexpr int kFrames = 48000;
    std::vector<float> input(static_cast<size_t>(kFrames * 2), 0.0f);
    std::vector<float> output(static_cast<size_t>((kFrames + 64) * 2), 0.0f);

    const int frames = resampler.process(input.data(), kFrames, 2, output.data(), kFrames + 64);

    EXPECT_GT(frames, kFrames);
    EXPECT_NEAR(frames, 48025, 2);
}

} // namespace
