// Stage 2 — AltsettingSelector unit tests
//
// Verifies the preference-driven altsetting scoring algorithm:
//  1. Prefers higher bit depth when bitDepthWeight is dominant.
//  2. Prefers async sync type over adaptive.
//  3. Fails gracefully when requireFeedback is set but no feedback available.
//  4. Respects minChannels constraint.
//  5. Deterministic tie-breaking (lowest alternateSetting wins).
//  6. Falls back when no UAC1 altsetting lists the requested rate.
//  7. UAC2 skips rate check (parser leaves sampleRates empty).
//  8. Picks the best format within a multi-format altsetting.

#include <gtest/gtest.h>

#include "../UsbAudioTypes.h"
#include "../AltsettingSelector.h"
#include "../StreamPreference.h"

using namespace watermelon_audio::usb;

namespace {

// ======== Helpers to build test topologies ========

UsbAudioFormat makeFormat(uint8_t channels, uint8_t bitRes, std::vector<int> rates = {}) {
    UsbAudioFormat fmt;
    fmt.channels = channels;
    fmt.bitResolution = bitRes;
    fmt.subframeSize = bitRes / 8;
    fmt.sampleRates = std::move(rates);
    return fmt;
}

UsbStreamingInterface makeAlt(uint8_t ifNum, uint8_t altNum, UsbAudioFormat fmt,
                               uint8_t syncType = 0x01, // async
                               bool hasFeedback = false) {
    UsbStreamingInterface alt;
    alt.interfaceNumber = ifNum;
    alt.alternateSetting = altNum;
    alt.formats.push_back(std::move(fmt));
    // Encode sync type into endpoint attributes: bits 3:2
    alt.dataEndpoint.attributes = 0x01 | (syncType << 2);  // isochronous
    alt.dataEndpoint.address = 0x01;  // output
    alt.dataEndpoint.maxPacketSize = 576;
    if (hasFeedback) {
        UsbFeedbackEndpoint fb;
        fb.endpoint.address = 0x81;  // input
        fb.isImplicit = false;
        alt.feedbackEndpoint = fb;
    }
    return alt;
}

UsbAudioDevice makeDevice(int uacVersion = 1) {
    UsbAudioDevice device;
    device.uacVersion = static_cast<uint8_t>(uacVersion);
    return device;
}

} // namespace

// ---- Test: prefer higher bit depth ----
TEST(AltsettingSelector, PrefersHigherBitDepth) {
    auto device = makeDevice(1);
    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(2, 16, {48000})));
    device.playbackInterfaces.push_back(
        makeAlt(1, 2, makeFormat(2, 24, {48000})));

    StreamPreference pref = StreamPreference::defaultPro();
    pref.requiredSampleRate = 48000;

    auto match = AltsettingSelector::pickPlayback(device, pref);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->format->bitResolution, 24);
    EXPECT_EQ(match->altsetting->alternateSetting, 2);
}

// ---- Test: prefer async over adaptive ----
TEST(AltsettingSelector, PrefersAsyncOverAdaptive) {
    auto device = makeDevice(1);
    // Alt 1: adaptive (syncType=2), 24-bit
    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(2, 24, {48000}), 0x02));
    // Alt 2: async (syncType=1), 24-bit
    device.playbackInterfaces.push_back(
        makeAlt(1, 2, makeFormat(2, 24, {48000}), 0x01));

    StreamPreference pref = StreamPreference::defaultPro();
    pref.requiredSampleRate = 48000;

    auto match = AltsettingSelector::pickPlayback(device, pref);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->altsetting->alternateSetting, 2);  // async wins
}

// ---- Test: requireFeedback filters correctly ----
TEST(AltsettingSelector, FailsWhenRequireFeedbackNotMet) {
    auto device = makeDevice(1);
    // No feedback on either altsetting
    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(2, 24, {48000}), 0x02, false));
    device.playbackInterfaces.push_back(
        makeAlt(1, 2, makeFormat(2, 16, {48000}), 0x01, false));

    StreamPreference pref;
    pref.requiredSampleRate = 48000;
    pref.requireFeedback = true;

    auto match = AltsettingSelector::pickPlayback(device, pref);
    EXPECT_FALSE(match.has_value());
}

// ---- Test: requireFeedback passes when available ----
TEST(AltsettingSelector, SelectsAltsettingWithFeedback) {
    auto device = makeDevice(1);
    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(2, 24, {48000}), 0x02, false));
    device.playbackInterfaces.push_back(
        makeAlt(1, 2, makeFormat(2, 16, {48000}), 0x01, true));

    StreamPreference pref;
    pref.requiredSampleRate = 48000;
    pref.requireFeedback = true;

    auto match = AltsettingSelector::pickPlayback(device, pref);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->altsetting->alternateSetting, 2);
}

// ---- Test: minChannels constraint ----
TEST(AltsettingSelector, RespectsMinChannels) {
    auto device = makeDevice(1);
    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(1, 24, {48000})));  // mono
    device.playbackInterfaces.push_back(
        makeAlt(1, 2, makeFormat(2, 16, {48000})));  // stereo

    StreamPreference pref;
    pref.requiredSampleRate = 48000;
    pref.minChannels = 2;

    auto match = AltsettingSelector::pickPlayback(device, pref);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->format->channels, 2);
}

// ---- Test: deterministic tie-breaking ----
TEST(AltsettingSelector, DeterministicOnTie) {
    auto device = makeDevice(1);
    // Two identical altsettings with different alt numbers
    device.playbackInterfaces.push_back(
        makeAlt(1, 3, makeFormat(2, 24, {48000})));
    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(2, 24, {48000})));

    StreamPreference pref = StreamPreference::defaultPro();
    pref.requiredSampleRate = 48000;

    auto m1 = AltsettingSelector::pickPlayback(device, pref);
    auto m2 = AltsettingSelector::pickPlayback(device, pref);
    ASSERT_TRUE(m1.has_value() && m2.has_value());
    // Lowest alternateSetting wins on tie
    EXPECT_EQ(m1->altsetting->alternateSetting, 1);
    EXPECT_EQ(m2->altsetting->alternateSetting, 1);
}

// ---- Test: UAC1 no rate match → empty result ----
TEST(AltsettingSelector, FallbackWhenNoRateMatch_UAC1) {
    auto device = makeDevice(1);
    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(2, 24, {44100})));

    StreamPreference pref;
    pref.requiredSampleRate = 96000;

    auto match = AltsettingSelector::pickPlayback(device, pref);
    EXPECT_FALSE(match.has_value());
}

// ---- Test: UAC2 skips rate check ----
TEST(AltsettingSelector, UAC2SkipsRateCheck) {
    auto device = makeDevice(2);
    // UAC2 altsetting with empty sampleRates (normal — rates from clock source)
    device.playbackInterfaces.push_back(
        makeAlt(1, 1, makeFormat(2, 24)));

    StreamPreference pref;
    pref.requiredSampleRate = 96000;

    auto match = AltsettingSelector::pickPlayback(device, pref);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->format->bitResolution, 24);
}

// ---- Test: multi-format altsetting ----
TEST(AltsettingSelector, MultiFormatAltsetting) {
    auto device = makeDevice(1);
    UsbStreamingInterface alt;
    alt.interfaceNumber = 1;
    alt.alternateSetting = 1;
    alt.formats.push_back(makeFormat(2, 16, {48000}));
    alt.formats.push_back(makeFormat(2, 24, {48000}));
    alt.dataEndpoint.attributes = 0x05;  // isochronous async
    alt.dataEndpoint.address = 0x01;
    alt.dataEndpoint.maxPacketSize = 576;
    device.playbackInterfaces.push_back(alt);

    StreamPreference pref = StreamPreference::defaultPro();
    pref.requiredSampleRate = 48000;

    auto match = AltsettingSelector::pickPlayback(device, pref);
    ASSERT_TRUE(match.has_value());
    // Should pick the 24-bit format
    EXPECT_EQ(match->format->bitResolution, 24);
    EXPECT_EQ(match->altsetting->alternateSetting, 1);
}

// ---- Test: capture direction works too ----
TEST(AltsettingSelector, CaptureSelection) {
    auto device = makeDevice(1);
    UsbStreamingInterface alt;
    alt.interfaceNumber = 2;
    alt.alternateSetting = 1;
    alt.formats.push_back(makeFormat(1, 16, {48000}));
    alt.dataEndpoint.attributes = 0x05;
    alt.dataEndpoint.address = 0x82;  // input
    alt.dataEndpoint.maxPacketSize = 192;
    device.captureInterfaces.push_back(alt);

    StreamPreference pref;
    pref.requiredSampleRate = 48000;
    pref.minChannels = 1;

    auto match = AltsettingSelector::pickCapture(device, pref);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->format->channels, 1);
}
