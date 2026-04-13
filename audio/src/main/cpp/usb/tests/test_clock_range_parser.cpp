// Stage 3 — ClockSourceRangeParser unit tests
//
// Verifies the pure-function RANGE response parser used by
// LibusbBackend::populateClockSourceRates(). Tests exercise:
//
//  1. Decoding numSubRanges and each sub-range triplet
//  2. Discrete rate detection (min == max)
//  3. Continuous range detection (min < max) and common-rate enumeration
//  4. Defensive parsing for truncated / malformed buffers
//  5. Application to a UsbClockSource populates min/max/rates correctly

#include <gtest/gtest.h>

#include "../ClockSourceRangeParser.h"
#include "../UsbAudioTypes.h"

using namespace watermelon_audio::usb;

namespace {

// Helper: append a little-endian u32 to a byte vector.
void appendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// Build a canonical RANGE response buffer: numSubRanges u16 + N*(min,max,res) u32 triplets
std::vector<uint8_t> buildRangeBuffer(
    std::initializer_list<std::tuple<uint32_t, uint32_t, uint32_t>> subRanges) {
    std::vector<uint8_t> buf;
    uint16_t n = static_cast<uint16_t>(subRanges.size());
    buf.push_back(static_cast<uint8_t>(n & 0xFF));
    buf.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    for (const auto& t : subRanges) {
        appendU32LE(buf, std::get<0>(t));
        appendU32LE(buf, std::get<1>(t));
        appendU32LE(buf, std::get<2>(t));
    }
    return buf;
}

} // namespace

// ---- Discrete rates (min==max in each sub-range) ----
TEST(ClockRangeParser, DiscreteRates_44100_48000_96000) {
    auto buf = buildRangeBuffer({
        {44100, 44100, 0},
        {48000, 48000, 0},
        {96000, 96000, 0},
    });
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());
    ASSERT_EQ(ranges.size(), 3u);
    EXPECT_EQ(ranges[0].minHz, 44100u);
    EXPECT_EQ(ranges[0].maxHz, 44100u);
    EXPECT_TRUE(ranges[0].isDiscrete());
    EXPECT_EQ(ranges[2].minHz, 96000u);
    EXPECT_TRUE(ranges[2].isDiscrete());
}

// ---- Continuous range (one big span) ----
TEST(ClockRangeParser, ContinuousRange_8000_to_192000) {
    auto buf = buildRangeBuffer({
        {8000, 192000, 1},
    });
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_FALSE(ranges[0].isDiscrete());
    EXPECT_EQ(ranges[0].minHz, 8000u);
    EXPECT_EQ(ranges[0].maxHz, 192000u);
}

// ---- Mixed (common on real devices) ----
TEST(ClockRangeParser, MixedDiscreteAndContinuous) {
    auto buf = buildRangeBuffer({
        {44100, 44100, 0},
        {48000, 96000, 100},   // continuous span
    });
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_TRUE(ranges[0].isDiscrete());
    EXPECT_FALSE(ranges[1].isDiscrete());
}

// ---- Empty / malformed ----
TEST(ClockRangeParser, NullBufferReturnsEmpty) {
    auto ranges = parseClockRangeResponse(nullptr, 0);
    EXPECT_TRUE(ranges.empty());
}

TEST(ClockRangeParser, TooShortBufferReturnsEmpty) {
    uint8_t tiny[1] = {0};
    auto ranges = parseClockRangeResponse(tiny, 1);
    EXPECT_TRUE(ranges.empty());
}

TEST(ClockRangeParser, HeaderDeclaresMoreThanPayloadReturnsEmpty) {
    // numSubRanges=2 but only 1 triplet of bytes
    std::vector<uint8_t> buf = {0x02, 0x00};
    appendU32LE(buf, 48000);
    appendU32LE(buf, 48000);
    appendU32LE(buf, 0);
    // only 14 bytes total, expected 26
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());
    EXPECT_TRUE(ranges.empty());
}

TEST(ClockRangeParser, ZeroSubRangesHeaderIsValid) {
    std::vector<uint8_t> buf = {0x00, 0x00};
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());
    EXPECT_TRUE(ranges.empty());
}

// ---- applyRangesToClockSource ----
TEST(ClockRangeParser, ApplyDiscreteRatesToClockSource) {
    auto buf = buildRangeBuffer({
        {44100, 44100, 0},
        {48000, 48000, 0},
        {96000, 96000, 0},
    });
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());

    UsbClockSource cs;
    applyRangesToClockSource(ranges, cs);

    EXPECT_FALSE(cs.hasContinuousRates);
    EXPECT_EQ(cs.minSampleRate, 44100);
    EXPECT_EQ(cs.maxSampleRate, 96000);
    ASSERT_EQ(cs.sampleRates.size(), 3u);
    EXPECT_EQ(cs.sampleRates[0], 44100);
    EXPECT_EQ(cs.sampleRates[1], 48000);
    EXPECT_EQ(cs.sampleRates[2], 96000);
}

TEST(ClockRangeParser, ApplyContinuousRangeEnumeratesCommonRates) {
    // UGREEN CM720 typically reports ~8000..192000 continuous
    auto buf = buildRangeBuffer({
        {8000, 192000, 1},
    });
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());

    UsbClockSource cs;
    applyRangesToClockSource(ranges, cs);

    EXPECT_TRUE(cs.hasContinuousRates);
    EXPECT_EQ(cs.minSampleRate, 8000);
    EXPECT_EQ(cs.maxSampleRate, 192000);
    // Common rates enumerated inside the span
    ASSERT_FALSE(cs.sampleRates.empty());
    bool has44100 = false, has48000 = false, has96000 = false, has192000 = false;
    for (int r : cs.sampleRates) {
        if (r == 44100) has44100 = true;
        if (r == 48000) has48000 = true;
        if (r == 96000) has96000 = true;
        if (r == 192000) has192000 = true;
    }
    EXPECT_TRUE(has44100);
    EXPECT_TRUE(has48000);
    EXPECT_TRUE(has96000);
    EXPECT_TRUE(has192000);
}

TEST(ClockRangeParser, ApplyNarrowContinuousRangeExcludesOutOfBounds) {
    // 44100..48000 only — should NOT include 96000
    auto buf = buildRangeBuffer({
        {44100, 48000, 1},
    });
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());

    UsbClockSource cs;
    applyRangesToClockSource(ranges, cs);

    EXPECT_TRUE(cs.hasContinuousRates);
    for (int r : cs.sampleRates) {
        EXPECT_GE(r, 44100);
        EXPECT_LE(r, 48000);
    }
}

TEST(ClockRangeParser, SampleRatesSortedAscending) {
    // Feed out-of-order to verify sorting
    auto buf = buildRangeBuffer({
        {96000, 96000, 0},
        {44100, 44100, 0},
        {48000, 48000, 0},
    });
    auto ranges = parseClockRangeResponse(buf.data(), buf.size());

    UsbClockSource cs;
    applyRangesToClockSource(ranges, cs);

    ASSERT_EQ(cs.sampleRates.size(), 3u);
    EXPECT_EQ(cs.sampleRates[0], 44100);
    EXPECT_EQ(cs.sampleRates[1], 48000);
    EXPECT_EQ(cs.sampleRates[2], 96000);
}
