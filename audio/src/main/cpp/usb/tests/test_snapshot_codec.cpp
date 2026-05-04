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

UsbStreamingInterface makeLogAlt(uint8_t ifNum, uint8_t altNum, uint8_t endpoint,
                                  uint8_t attributes, uint16_t maxPacket,
                                  uint8_t terminalLink, UsbAudioFormat fmt) {
    UsbStreamingInterface alt;
    alt.interfaceNumber = ifNum;
    alt.alternateSetting = altNum;
    alt.formats.push_back(std::move(fmt));
    alt.dataEndpoint.address = endpoint;
    alt.dataEndpoint.attributes = attributes;
    alt.dataEndpoint.maxPacketSize = maxPacket;
    alt.dataEndpoint.interval = 1;
    alt.terminalLink = terminalLink;
    return alt;
}

UsbAudioDevice makeGhwUac1DiscoveryLogFixture() {
    UsbAudioDevice device;
    device.deviceInfo.vendorId = 0x31B2;
    device.deviceInfo.productId = 0x0011;
    device.deviceInfo.manufacturer = "GHW Micro";
    device.deviceInfo.product = "GHW USB AUDIO";
    device.uacVersion = 1;

    // Derived from docs/usb-audio/descovery_test_results.md:
    // IF1 Alt1 capture 1ch/16bit 48 kHz, IF2 Alt1/2 playback 2ch 16/24bit.
    device.captureInterfaces.push_back(
        makeLogAlt(1, 1, 0x81, 0x05, 96, 4, makeFormat(1, 16, {48000})));
    device.playbackInterfaces.push_back(
        makeLogAlt(2, 1, 0x01, 0x09, 384, 5, makeFormat(2, 16, {48000, 96000})));
    device.playbackInterfaces.push_back(
        makeLogAlt(2, 2, 0x01, 0x09, 576, 5, makeFormat(2, 24, {48000, 96000})));

    UsbFeatureUnit playbackVolume;
    playbackVolume.unitId = 2;
    playbackVolume.sourceId = 1;
    playbackVolume.numChannels = 0;
    playbackVolume.channelControls = {0x03};
    device.featureUnits.push_back(playbackVolume);

    UsbFeatureUnit captureVolume;
    captureVolume.unitId = 7;
    captureVolume.sourceId = 6;
    captureVolume.numChannels = 1;
    captureVolume.channelControls = {0x01, 0x02};
    device.featureUnits.push_back(captureVolume);

    return device;
}

UsbAudioDevice makeCm720Uac2DiscoveryLogFixture() {
    UsbAudioDevice device;
    device.deviceInfo.vendorId = 0x2B89;
    device.deviceInfo.productId = 0x64EC;
    device.deviceInfo.manufacturer = "Realtek";
    device.deviceInfo.product = "UGREEN CM720 USB Audio";
    device.uacVersion = 2;

    // Derived from docs/usb-audio/descovery_test_results.md:
    // UAC2 exposes one capture altsetting and three adaptive playback altsettings.
    device.captureInterfaces.push_back(
        makeLogAlt(1, 1, 0x81, 0x05, 28, 2, makeFormat(2, 16)));
    device.playbackInterfaces.push_back(
        makeLogAlt(2, 1, 0x07, 0x09, 248, 14, makeFormat(2, 16)));
    device.playbackInterfaces.push_back(
        makeLogAlt(2, 2, 0x07, 0x09, 372, 14, makeFormat(2, 24)));
    device.playbackInterfaces.push_back(
        makeLogAlt(2, 3, 0x07, 0x09, 496, 14, makeFormat(2, 32)));

    UsbClockSource playbackClock;
    playbackClock.clockId = 27;
    playbackClock.type = ClockSourceType::INTERNAL_PROGRAMMABLE;
    playbackClock.syncedToSof = true;
    playbackClock.canControlFrequency = true;
    device.clockSources.push_back(playbackClock);

    UsbClockSource captureClock;
    captureClock.clockId = 30;
    captureClock.type = ClockSourceType::INTERNAL_PROGRAMMABLE;
    captureClock.syncedToSof = true;
    captureClock.canControlFrequency = true;
    device.clockSources.push_back(captureClock);

    return device;
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

// Stage 3: clock sources now carry sample rate lists from the RANGE query.
TEST(UsbSnapshotCodec, RoundTripWithClockSourceSampleRates) {
    UsbAudioDevice device;
    device.uacVersion = 2;

    UsbClockSource cs;
    cs.clockId = 30;
    cs.type = ClockSourceType::INTERNAL_PROGRAMMABLE;
    cs.canControlFrequency = true;
    cs.hasContinuousRates = false;
    cs.sampleRates = {44100, 48000, 88200, 96000};
    cs.minSampleRate = 44100;
    cs.maxSampleRate = 96000;
    device.clockSources.push_back(cs);

    auto encoded = encodeSnapshot(device);
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    ASSERT_EQ(decoded->clockSources.size(), 1u);
    const auto& dcs = decoded->clockSources[0];
    EXPECT_EQ(dcs.clockId, 30);
    EXPECT_FALSE(dcs.hasContinuousRates);
    EXPECT_EQ(dcs.minSampleRate, 44100);
    EXPECT_EQ(dcs.maxSampleRate, 96000);
    ASSERT_EQ(dcs.sampleRates.size(), 4u);
    EXPECT_EQ(dcs.sampleRates[0], 44100);
    EXPECT_EQ(dcs.sampleRates[3], 96000);
}

TEST(UsbSnapshotCodec, RoundTripWithContinuousClockSource) {
    UsbAudioDevice device;
    device.uacVersion = 2;

    UsbClockSource cs;
    cs.clockId = 1;
    cs.type = ClockSourceType::INTERNAL_VARIABLE;
    cs.canControlFrequency = true;
    cs.hasContinuousRates = true;
    cs.minSampleRate = 8000;
    cs.maxSampleRate = 192000;
    // Empty discrete list — continuous range has no explicit rates
    device.clockSources.push_back(cs);

    auto encoded = encodeSnapshot(device);
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    ASSERT_EQ(decoded->clockSources.size(), 1u);
    EXPECT_TRUE(decoded->clockSources[0].hasContinuousRates);
    EXPECT_EQ(decoded->clockSources[0].minSampleRate, 8000);
    EXPECT_EQ(decoded->clockSources[0].maxSampleRate, 192000);
    EXPECT_TRUE(decoded->clockSources[0].sampleRates.empty());
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

TEST(UsbSnapshotCodec, RoundTripGhwUac1DiscoveryLogFixture) {
    auto encoded = encodeSnapshot(makeGhwUac1DiscoveryLogFixture());
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(decoded->deviceInfo.vendorId, 0x31B2);
    EXPECT_EQ(decoded->deviceInfo.productId, 0x0011);
    EXPECT_EQ(decoded->deviceInfo.manufacturer, "GHW Micro");
    EXPECT_EQ(decoded->deviceInfo.product, "GHW USB AUDIO");
    EXPECT_EQ(decoded->uacVersion, 1);

    ASSERT_EQ(decoded->playbackInterfaces.size(), 2u);
    EXPECT_EQ(decoded->playbackInterfaces[0].interfaceNumber, 2);
    EXPECT_EQ(decoded->playbackInterfaces[0].alternateSetting, 1);
    EXPECT_EQ(decoded->playbackInterfaces[0].primaryFormat().bitResolution, 16);
    EXPECT_EQ(decoded->playbackInterfaces[1].alternateSetting, 2);
    EXPECT_EQ(decoded->playbackInterfaces[1].primaryFormat().bitResolution, 24);
    ASSERT_EQ(decoded->playbackInterfaces[1].primaryFormat().sampleRates.size(), 2u);
    EXPECT_EQ(decoded->playbackInterfaces[1].primaryFormat().sampleRates[1], 96000);
    EXPECT_EQ(decoded->playbackInterfaces[1].terminalLink, 5);

    ASSERT_EQ(decoded->captureInterfaces.size(), 1u);
    EXPECT_EQ(decoded->captureInterfaces[0].interfaceNumber, 1);
    EXPECT_EQ(decoded->captureInterfaces[0].primaryFormat().channels, 1);
    EXPECT_EQ(decoded->captureInterfaces[0].primaryFormat().bitResolution, 16);
    EXPECT_EQ(decoded->captureInterfaces[0].terminalLink, 4);
    EXPECT_EQ(decoded->featureUnits.size(), 2u);
}

TEST(UsbSnapshotCodec, RoundTripCm720Uac2DiscoveryLogFixture) {
    auto encoded = encodeSnapshot(makeCm720Uac2DiscoveryLogFixture());
    auto decoded = decodeSnapshot(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(decoded->deviceInfo.vendorId, 0x2B89);
    EXPECT_EQ(decoded->deviceInfo.productId, 0x64EC);
    EXPECT_EQ(decoded->deviceInfo.manufacturer, "Realtek");
    EXPECT_EQ(decoded->deviceInfo.product, "UGREEN CM720 USB Audio");
    EXPECT_EQ(decoded->uacVersion, 2);

    ASSERT_EQ(decoded->playbackInterfaces.size(), 3u);
    EXPECT_EQ(decoded->playbackInterfaces[0].alternateSetting, 1);
    EXPECT_EQ(decoded->playbackInterfaces[0].primaryFormat().bitResolution, 16);
    EXPECT_EQ(decoded->playbackInterfaces[1].alternateSetting, 2);
    EXPECT_EQ(decoded->playbackInterfaces[1].primaryFormat().bitResolution, 24);
    EXPECT_EQ(decoded->playbackInterfaces[2].alternateSetting, 3);
    EXPECT_EQ(decoded->playbackInterfaces[2].primaryFormat().bitResolution, 32);
    EXPECT_EQ(decoded->playbackInterfaces[2].terminalLink, 14);

    ASSERT_EQ(decoded->captureInterfaces.size(), 1u);
    EXPECT_EQ(decoded->captureInterfaces[0].primaryFormat().channels, 2);
    EXPECT_EQ(decoded->captureInterfaces[0].primaryFormat().bitResolution, 16);

    ASSERT_EQ(decoded->clockSources.size(), 2u);
    EXPECT_EQ(decoded->clockSources[0].clockId, 27);
    EXPECT_EQ(decoded->clockSources[0].type, ClockSourceType::INTERNAL_PROGRAMMABLE);
    EXPECT_TRUE(decoded->clockSources[0].syncedToSof);
    EXPECT_TRUE(decoded->clockSources[0].canControlFrequency);
    EXPECT_EQ(decoded->clockSources[1].clockId, 30);
}

TEST(UsbSnapshotCodec, RejectsTruncatedBuffer) {
    auto decoded = decodeSnapshot(nullptr, 0);
    EXPECT_FALSE(decoded.has_value());

    uint8_t tiny[5] = {0x01, 0, 0, 0, 0};
    decoded = decodeSnapshot(tiny, 5);
    EXPECT_FALSE(decoded.has_value());
}
