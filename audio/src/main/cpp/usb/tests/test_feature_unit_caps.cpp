#include "../UsbAudioTypes.h"

#include <gtest/gtest.h>

using namespace watermelon_audio::usb;

namespace {

TEST(FeatureUnitCapsTest, Uac1ReportsMasterAndPerChannelControlsSeparately) {
    UsbFeatureUnit fu;
    fu.unitId = 5;
    fu.sourceId = 1;
    fu.numChannels = 2;
    fu.channelControls = {
        0x01, // master mute only
        0x02, // channel 1 volume only
        0x03, // channel 2 volume + mute
    };

    EXPECT_FALSE(fu.hasVolumeControl(0, 1));
    EXPECT_TRUE(fu.hasMuteControl(0, 1));
    EXPECT_TRUE(fu.hasVolumeControl(1, 1));
    EXPECT_FALSE(fu.hasMuteControl(1, 1));
    EXPECT_TRUE(fu.hasVolumeControl(2, 1));
    EXPECT_TRUE(fu.hasMuteControl(2, 1));

    EXPECT_EQ(fu.volumeControlChannels(1), (std::vector<uint8_t>{1, 2}));
    EXPECT_EQ(fu.muteControlChannels(1), (std::vector<uint8_t>{0, 2}));
}

TEST(FeatureUnitCapsTest, Uac2ReportsTwoBitControlFieldsPerChannel) {
    UsbFeatureUnit fu;
    fu.unitId = 9;
    fu.sourceId = 2;
    fu.numChannels = 2;
    fu.channelControls = {
        0x00000003, // master mute present
        0x0000000C, // channel 1 volume present
        0x0000000F, // channel 2 mute + volume present
    };

    EXPECT_FALSE(fu.hasVolumeControl(0, 2));
    EXPECT_TRUE(fu.hasMuteControl(0, 2));
    EXPECT_TRUE(fu.hasVolumeControl(1, 2));
    EXPECT_FALSE(fu.hasMuteControl(1, 2));
    EXPECT_TRUE(fu.hasVolumeControl(2, 2));
    EXPECT_TRUE(fu.hasMuteControl(2, 2));

    EXPECT_EQ(fu.volumeControlChannels(2), (std::vector<uint8_t>{1, 2}));
    EXPECT_EQ(fu.muteControlChannels(2), (std::vector<uint8_t>{0, 2}));
}

TEST(FeatureUnitCapsTest, OutOfRangeChannelHasNoHardwareControl) {
    UsbFeatureUnit fu;
    fu.channelControls = {0x03};

    EXPECT_FALSE(fu.hasVolumeControl(1, 1));
    EXPECT_FALSE(fu.hasMuteControl(1, 1));
    EXPECT_EQ(fu.volumeControlChannels(1), (std::vector<uint8_t>{0}));
    EXPECT_EQ(fu.muteControlChannels(1), (std::vector<uint8_t>{0}));
}

} // namespace
