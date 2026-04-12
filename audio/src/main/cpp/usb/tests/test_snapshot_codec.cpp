// Stage 2 — UsbSnapshotCodec round-trip tests
//
// Verifies that:
//  1. A simple device survives encode→decode round-trip with all fields intact.
//  2. Multiple formats per altsetting are preserved.
//  3. Clock sources survive round-trip.
//  4. Feature units survive round-trip.
//  5. Empty device encodes/decodes without crashing.
//  6. Version byte is correctly set to 0x01.
//  7. Decode rejects unknown version.

#include <gtest/gtest.h>

#include "../UsbSnapshotCodec.h"

using namespace watermelon_audio::usb;

namespace {

UsbAudioFormat makeFormat(uint8_t channels, uint8_t bitRes, std::vector<int> rates = {}) {
    UsbAudioFormat fmt;
    fmt.channels = channels;
    fmt.bitResolution = bitRes;
    fmt.subframeSize = bitRes / 8;
    fmt.sampleRates = std::move(rates);
    return fmt;
}

UsbStreamingInterface makeAlt(uint8_t ifNum, uint8_t altNum, UsbAudioFormat fmt,
                               bool hasFeedback = false) {
    UsbStreamingInterface alt;
    alt.interfaceNumber = ifNum;
    alt.alternateSetting = altNum;
    alt.formats.push_back(std::move(fmt));
    alt.dataEndpoint.attributes = 0x05;  // isochronous async
    alt.dataEndpoint.address = 0x01;
    alt.dataEndpoint.maxPacketSize = 576;
    alt.terminalLink = 3;
    if (hasFeedback) {
        UsbFeedbackEndpoint fb;
        fb.endpoint.address = 0x81;
        fb.isImplicit = false;
        alt.feedbackEndpoint = fb;
    }
    return alt;
}

} // namespace

TEST(UsbSnapshotCodec, RoundTripSimpleDevice) {
    UsbAudioDevice device;
    device.deviceInfo.vendorId = 0x31B2;
    device.deviceInfo.productId = 0x0011;
    device.deviceInfo.product = "GHW USB AUDIO";
    device.deviceInfo.manufacturer = "GHW";
    device.deviceInfo.serialNumber = "SN123";
    device.uacVersion = 1;

    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(2, 16, {44100, 48000})));
    device.playbackInterfaces.push_back(
        makeAlt(1, 2, makeFormat(2, 24, {44100, 48000}), true));

    device.captureInterfaces.push_back(
        makeAlt(2, 1, makeFormat(1, 16, {48000})));

    auto encoded = encodeSnapshot(device);
    ASSERT_GT(encoded.size(), 10u);

    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(decoded->deviceInfo.vendorId, 0x31B2);
    EXPECT_EQ(decoded->deviceInfo.productId, 0x0011);
    EXPECT_EQ(decoded->deviceInfo.product, "GHW USB AUDIO");
    EXPECT_EQ(decoded->deviceInfo.manufacturer, "GHW");
    EXPECT_EQ(decoded->deviceInfo.serialNumber, "SN123");
    EXPECT_EQ(decoded->uacVersion, 1);

    ASSERT_EQ(decoded->playbackInterfaces.size(), 2u);
    EXPECT_EQ(decoded->playbackInterfaces[0].alternateSetting, 1);
    EXPECT_EQ(decoded->playbackInterfaces[0].primaryFormat().channels, 2);
    EXPECT_EQ(decoded->playbackInterfaces[0].primaryFormat().bitResolution, 16);
    ASSERT_EQ(decoded->playbackInterfaces[0].primaryFormat().sampleRates.size(), 2u);
    EXPECT_EQ(decoded->playbackInterfaces[0].primaryFormat().sampleRates[0], 44100);
    EXPECT_EQ(decoded->playbackInterfaces[0].primaryFormat().sampleRates[1], 48000);
    EXPECT_FALSE(decoded->playbackInterfaces[0].feedbackEndpoint.has_value());

    EXPECT_EQ(decoded->playbackInterfaces[1].primaryFormat().bitResolution, 24);
    EXPECT_TRUE(decoded->playbackInterfaces[1].feedbackEndpoint.has_value());

    ASSERT_EQ(decoded->captureInterfaces.size(), 1u);
    EXPECT_EQ(decoded->captureInterfaces[0].primaryFormat().channels, 1);
    EXPECT_EQ(decoded->captureInterfaces[0].primaryFormat().bitResolution, 16);
}

TEST(UsbSnapshotCodec, RoundTripWithMultipleFormats) {
    UsbAudioDevice device;
    device.deviceInfo.vendorId = 0x1234;
    device.deviceInfo.productId = 0x5678;
    device.uacVersion = 1;

    UsbStreamingInterface alt;
    alt.interfaceNumber = 1;
    alt.alternateSetting = 1;
    alt.formats.push_back(makeFormat(2, 16, {48000}));
    alt.formats.push_back(makeFormat(2, 24, {48000, 96000}));
    alt.dataEndpoint.attributes = 0x05;
    alt.dataEndpoint.address = 0x01;
    alt.dataEndpoint.maxPacketSize = 576;
    device.playbackInterfaces.push_back(alt);

    auto encoded = encodeSnapshot(device);
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    ASSERT_EQ(decoded->playbackInterfaces.size(), 1u);
    ASSERT_EQ(decoded->playbackInterfaces[0].formats.size(), 2u);
    EXPECT_EQ(decoded->playbackInterfaces[0].formats[0].bitResolution, 16);
    EXPECT_EQ(decoded->playbackInterfaces[0].formats[1].bitResolution, 24);
    ASSERT_EQ(decoded->playbackInterfaces[0].formats[1].sampleRates.size(), 2u);
    EXPECT_EQ(decoded->playbackInterfaces[0].formats[1].sampleRates[0], 48000);
    EXPECT_EQ(decoded->playbackInterfaces[0].formats[1].sampleRates[1], 96000);
}

TEST(UsbSnapshotCodec, RoundTripWithClockSources) {
    UsbAudioDevice device;
    device.deviceInfo.vendorId = 0x2B89;
    device.deviceInfo.productId = 0x64EC;
    device.uacVersion = 2;

    UsbClockSource cs;
    cs.clockId = 27;
    cs.type = ClockSourceType::INTERNAL_PROGRAMMABLE;
    cs.syncedToSof = false;
    cs.canControlFrequency = true;
    cs.hasValidityControl = true;
    device.clockSources.push_back(cs);

    auto encoded = encodeSnapshot(device);
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    ASSERT_EQ(decoded->clockSources.size(), 1u);
    EXPECT_EQ(decoded->clockSources[0].clockId, 27);
    EXPECT_EQ(decoded->clockSources[0].type, ClockSourceType::INTERNAL_PROGRAMMABLE);
    EXPECT_FALSE(decoded->clockSources[0].syncedToSof);
    EXPECT_TRUE(decoded->clockSources[0].canControlFrequency);
    EXPECT_TRUE(decoded->clockSources[0].hasValidityControl);
}

TEST(UsbSnapshotCodec, RoundTripWithFeatureUnits) {
    UsbAudioDevice device;
    device.uacVersion = 1;

    UsbFeatureUnit fu;
    fu.unitId = 5;
    fu.sourceId = 1;
    fu.numChannels = 2;
    // Master + 2 channels
    fu.channelControls = {0x03, 0x03, 0x03};  // volume + mute on all
    device.featureUnits.push_back(fu);

    auto encoded = encodeSnapshot(device);
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    ASSERT_EQ(decoded->featureUnits.size(), 1u);
    EXPECT_EQ(decoded->featureUnits[0].unitId, 5);
    EXPECT_EQ(decoded->featureUnits[0].sourceId, 1);
    EXPECT_EQ(decoded->featureUnits[0].numChannels, 2);
}

TEST(UsbSnapshotCodec, EmptyDevice) {
    UsbAudioDevice device;
    device.uacVersion = 1;

    auto encoded = encodeSnapshot(device);
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    EXPECT_TRUE(decoded->playbackInterfaces.empty());
    EXPECT_TRUE(decoded->captureInterfaces.empty());
    EXPECT_TRUE(decoded->clockSources.empty());
    EXPECT_TRUE(decoded->featureUnits.empty());
}

TEST(UsbSnapshotCodec, VersionByte) {
    UsbAudioDevice device;
    auto encoded = encodeSnapshot(device);
    ASSERT_FALSE(encoded.empty());
    EXPECT_EQ(encoded[0], SNAPSHOT_FORMAT_VERSION);
    EXPECT_EQ(encoded[0], 0x01);
}

TEST(UsbSnapshotCodec, RejectsUnknownVersion) {
    UsbAudioDevice device;
    auto encoded = encodeSnapshot(device);
    // Corrupt version byte
    encoded[0] = 0xFF;
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    EXPECT_FALSE(decoded.has_value());
}

TEST(UsbSnapshotCodec, RejectsTruncatedBuffer) {
    auto decoded = decodeSnapshot(nullptr, 0);
    EXPECT_FALSE(decoded.has_value());

    uint8_t tiny[5] = {0x01, 0, 0, 0, 0};
    decoded = decodeSnapshot(tiny, 5);
    EXPECT_FALSE(decoded.has_value());
}
