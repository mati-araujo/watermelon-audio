// Stage 3 - USB iso pacing derived from device speed and bInterval.

#include <gtest/gtest.h>

#include "../UsbIsoTiming.h"

using namespace watermelon_audio::usb;

TEST(UsbIsoTimingTest, FullSpeedBIntervalUsesMillisecondFrames) {
    const auto timing = calculateIsoTransferTiming(
        48000,
        /*highSpeed=*/false,
        /*endpointInterval=*/1);

    EXPECT_EQ(timing.endpointInterval, 1);
    EXPECT_EQ(timing.packetsPerSecond, 1000);
    EXPECT_EQ(timing.framesPerPacket, 48);
    EXPECT_EQ(timing.packetsPerTransfer, 8);
}

TEST(UsbIsoTimingTest, HighSpeedBIntervalOneUsesMicroframes) {
    const auto timing = calculateIsoTransferTiming(
        48000,
        /*highSpeed=*/true,
        /*endpointInterval=*/1);

    EXPECT_EQ(timing.endpointInterval, 1);
    EXPECT_EQ(timing.packetsPerSecond, 8000);
    EXPECT_EQ(timing.framesPerPacket, 6);
    EXPECT_EQ(timing.packetsPerTransfer, 64);
}

TEST(UsbIsoTimingTest, HighSpeedBIntervalTwoHalvesServiceCadence) {
    const auto timing = calculateIsoTransferTiming(
        48000,
        /*highSpeed=*/true,
        /*endpointInterval=*/2);

    EXPECT_EQ(timing.endpointInterval, 2);
    EXPECT_EQ(timing.packetsPerSecond, 4000);
    EXPECT_EQ(timing.framesPerPacket, 12);
    EXPECT_EQ(timing.packetsPerTransfer, 32);
}

TEST(UsbIsoTimingTest, HighSpeedBIntervalFourUsesEightMicroframeServiceSlots) {
    const auto timing = calculateIsoTransferTiming(
        96000,
        /*highSpeed=*/true,
        /*endpointInterval=*/4);

    EXPECT_EQ(timing.endpointInterval, 4);
    EXPECT_EQ(timing.packetsPerSecond, 1000);
    EXPECT_EQ(timing.framesPerPacket, 96);
    EXPECT_EQ(timing.packetsPerTransfer, 8);
}

TEST(UsbIsoTimingTest, ZeroIntervalAndTinyRatesClampToSafeMinimums) {
    const auto timing = calculateIsoTransferTiming(
        1,
        /*highSpeed=*/true,
        /*endpointInterval=*/0);

    EXPECT_EQ(timing.endpointInterval, 1);
    EXPECT_EQ(timing.packetsPerSecond, 8000);
    EXPECT_EQ(timing.framesPerPacket, 1);
    EXPECT_EQ(timing.packetsPerTransfer, 64);
}
