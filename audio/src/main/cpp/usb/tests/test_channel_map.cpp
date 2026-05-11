#include "../ChannelMap.h"

#include <gtest/gtest.h>

#include <vector>

namespace watermelon_audio::usb {
namespace {

TEST(ChannelMapTest, IdentityStereoOutputIsBitEquivalent) {
    const auto map = ChannelMap::identity(2);
    const std::vector<float> input{
        0.10f, -0.20f,
        0.30f, -0.40f,
        0.50f, -0.60f,
    };
    std::vector<float> output(input.size(), 0.0f);

    ASSERT_TRUE(map.isOutputIdentity(2, 2));
    map.applyOutput(input.data(), output.data(), 3, 2, 2);

    EXPECT_EQ(output, input);
}

TEST(ChannelMapTest, SwapStereoOutput) {
    const auto map = ChannelMap::swapStereo();
    const std::vector<float> input{
        1.0f, 2.0f,
        3.0f, 4.0f,
    };
    std::vector<float> output(input.size(), 0.0f);

    ASSERT_FALSE(map.isOutputIdentity(2, 2));
    map.applyOutput(input.data(), output.data(), 2, 2, 2);

    EXPECT_EQ(output, (std::vector<float>{2.0f, 1.0f, 4.0f, 3.0f}));
}

TEST(ChannelMapTest, LeftOnlyDuplicatesLeftToStereo) {
    const auto map = ChannelMap::leftOnlyToStereo();
    const std::vector<float> input{
        0.25f, 0.75f,
        -0.50f, 0.125f,
    };
    std::vector<float> output(input.size(), 0.0f);

    map.applyOutput(input.data(), output.data(), 2, 2, 2);

    EXPECT_EQ(output, (std::vector<float>{0.25f, 0.25f, -0.50f, -0.50f}));
}

TEST(ChannelMapTest, MonoInputDownmixProducesStereoWithHeadroom) {
    const auto map = ChannelMap::monoToStereoDownmix();
    const std::vector<float> input{
        1.0f,
        -0.5f,
        0.25f,
    };
    std::vector<float> output(6, 0.0f);

    map.applyInput(input.data(), output.data(), 3, 1, 2);

    EXPECT_FLOAT_EQ(output[0], 0.707f);
    EXPECT_FLOAT_EQ(output[1], 0.707f);
    EXPECT_FLOAT_EQ(output[2], -0.3535f);
    EXPECT_FLOAT_EQ(output[3], -0.3535f);
    EXPECT_FLOAT_EQ(output[4], 0.17675f);
    EXPECT_FLOAT_EQ(output[5], 0.17675f);
}

TEST(ChannelMapTest, IdentityInputPreservesFourChannels) {
    const auto map = ChannelMap::identity(4);
    const std::vector<float> input{
        1.0f, 2.0f, 3.0f, 4.0f,
        5.0f, 6.0f, 7.0f, 8.0f,
    };
    std::vector<float> output(input.size(), 0.0f);

    ASSERT_TRUE(map.isInputIdentity(4, 4));
    map.applyInput(input.data(), output.data(), 2, 4, 4);

    EXPECT_EQ(output, input);
}

} // namespace
} // namespace watermelon_audio::usb
