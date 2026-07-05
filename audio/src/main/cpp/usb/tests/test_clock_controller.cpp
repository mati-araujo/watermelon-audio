// Fase 0.1 — ClockController unit tests
//
// The controller tracks the feedback value Ff *directly* (Ff is the setpoint,
// not an error against the nominal) and integrates the smoothed fractional
// target with a pure accumulator. These tests cover:
//   - parsing (10.14 / 16.16), kept from the original suite
//   - adjustment DIRECTION (the test that was missing): a device reported as
//     fast must make the host emit MORE frames, not fewer
//   - 44.1 kHz without feedback produces the correct fractional cadence
//   - the cadence conversion uses packetsPerSecond, not an assumed /8000
//   - clamp + anti-windup bounds the catch-up burst
//   - out-of-range feedback is rejected and leaves the target untouched

#include <gtest/gtest.h>

#include "../../backends/ClockController.h"

#include <array>
#include <cmath>
#include <cstdint>

using watermelon_audio::ClockController;
using watermelon_audio::UacVersion;

namespace {

// Encode a 10.14 fixed-point feedback value (UAC 1.0) into 3 little-endian
// bytes. `samplesPerFrame` = audio samples produced per 1 ms USB frame.
std::array<uint8_t, 3> encodeUac1Feedback(double samplesPerFrame) {
    const uint32_t raw = static_cast<uint32_t>(
        std::lround(samplesPerFrame * 16384.0));  // 2^14
    return {
        static_cast<uint8_t>(raw & 0xff),
        static_cast<uint8_t>((raw >> 8) & 0xff),
        static_cast<uint8_t>((raw >> 16) & 0xff),
    };
}

// Encode a 16.16 fixed-point feedback value (UAC 2.0) into 4 little-endian
// bytes. `samplesPerMicroframe` = audio samples produced per 125 µs microframe.
std::array<uint8_t, 4> encodeUac2Feedback(double samplesPerMicroframe) {
    const uint32_t raw = static_cast<uint32_t>(
        std::lround(samplesPerMicroframe * 65536.0));  // 2^16
    return {
        static_cast<uint8_t>(raw & 0xff),
        static_cast<uint8_t>((raw >> 8) & 0xff),
        static_cast<uint8_t>((raw >> 16) & 0xff),
        static_cast<uint8_t>((raw >> 24) & 0xff),
    };
}

// Push a steady UAC1 feedback value enough times for the EMA to converge.
void feedUac1(ClockController& ctrl, double samplesPerFrame, int times) {
    auto bytes = encodeUac1Feedback(samplesPerFrame);
    for (int i = 0; i < times; ++i) {
        ctrl.processFeedback(bytes.data(), 3, UacVersion::UAC_1_0);
    }
}

// Sum getAdjustedFrameCount over `packets` single-packet fills.
long long sumAdjusted(ClockController& ctrl, int nominalFrames, int packets) {
    long long total = 0;
    for (int i = 0; i < packets; ++i) {
        total += ctrl.getAdjustedFrameCount(nominalFrames, 1);
    }
    return total;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing (kept) — test 7 of the plan
// ---------------------------------------------------------------------------

TEST(ClockControllerTest, Uac1FeedbackParsedAsThreeBytes) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    feedUac1(ctrl, 48.0, 32);

    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
    EXPECT_LT(std::abs(ctrl.getDriftPpm()), 50.0f);
    EXPECT_TRUE(ctrl.isStable());
}

TEST(ClockControllerTest, Uac2FeedbackParsedAsFourBytes) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_2_0);

    // 6.0 samples per 125 µs microframe at 48 kHz; default cadence 1000 pkt/s.
    auto bytes = encodeUac2Feedback(6.0);
    for (int i = 0; i < 32; ++i) {
        ctrl.processFeedback(bytes.data(), 4, UacVersion::UAC_2_0);
    }

    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
    EXPECT_LT(std::abs(ctrl.getDriftPpm()), 50.0f);
    EXPECT_TRUE(ctrl.isStable());
}

TEST(ClockControllerTest, Uac1FeedbackThreeBytesIsNotMisparsedAsUac2) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    feedUac1(ctrl, 48.0, 32);

    EXPECT_GT(ctrl.getCurrentSampleRate(), 47000.0f);
    EXPECT_LT(ctrl.getCurrentSampleRate(), 49000.0f);
}

TEST(ClockControllerTest, RejectsTooShortPayload) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    uint8_t bogus[2] = {0xff, 0xff};
    ctrl.processFeedback(bogus, 2, UacVersion::UAC_1_0);
    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
    EXPECT_FALSE(ctrl.isStable());
}

TEST(ClockControllerTest, SetUacVersionResetsStateOnChange) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    // 48.5 samples/frame ≈ +1 % — within the ±10 % gate, so it is accepted
    // and produces a clearly non-zero drift.
    feedUac1(ctrl, 48.5, 16);
    EXPECT_GT(std::abs(ctrl.getDriftPpm()), 100.0f);

    ctrl.setUacVersion(UacVersion::UAC_2_0);
    EXPECT_FALSE(ctrl.isStable());
    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Test 1 — adjustment DIRECTION (was missing)
// ---------------------------------------------------------------------------

TEST(ClockControllerTest, FastDeviceMakesHostEmitMoreFrames) {
    // +100 PPM: device wants 48.0048 frames/packet. After convergence the host
    // must emit MORE frames, not fewer (the sign bug being fixed).
    ClockController ctrl;
    ctrl.configure(48000, 1000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    feedUac1(ctrl, 48.0048, 500);  // converge the EMA

    const long long total = sumAdjusted(ctrl, 48, 10000);
    EXPECT_NEAR(static_cast<double>(total), 48.0048 * 10000.0, 2.0);
}

TEST(ClockControllerTest, SlowDeviceMakesHostEmitFewerFrames) {
    // Mirror: -100 PPM → 47.9952 frames/packet.
    ClockController ctrl;
    ctrl.configure(48000, 1000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    feedUac1(ctrl, 47.9952, 500);

    const long long total = sumAdjusted(ctrl, 48, 10000);
    EXPECT_NEAR(static_cast<double>(total), 47.9952 * 10000.0, 2.0);
}

// ---------------------------------------------------------------------------
// Test 2 & 3 — 44.1 kHz without feedback (fractional nominal)
// ---------------------------------------------------------------------------

TEST(ClockControllerTest, FractionalNominal44100FullSpeed) {
    ClockController ctrl;
    ctrl.configure(44100, 1000);  // FS: 44.1 frames/packet
    ctrl.setUacVersion(UacVersion::UAC_1_0);
    // No feedback at all.

    const long long total = sumAdjusted(ctrl, 44, 1000);
    EXPECT_NEAR(static_cast<double>(total), 44100.0, 1.0);
    EXPECT_FALSE(ctrl.isStable());
}

TEST(ClockControllerTest, FractionalNominal44100HighSpeedBInterval1) {
    ClockController ctrl;
    ctrl.configure(44100, 8000);  // HS bInterval=1: 5.5125 frames/packet
    ctrl.setUacVersion(UacVersion::UAC_2_0);

    const long long total = sumAdjusted(ctrl, 5, 8000);
    EXPECT_NEAR(static_cast<double>(total), 44100.0, 1.0);
}

// ---------------------------------------------------------------------------
// Test 4 — cadence conversion uses packetsPerSecond, not /8000
// ---------------------------------------------------------------------------

TEST(ClockControllerTest, Uac2HighSpeedUsesPacketsPerSecondNotEightThousand) {
    // UAC2 device on bInterval=4 → 1000 packets/s, not 8000. Ff is still in
    // samples/microframe (6.0 at 48 kHz). A correct conversion gives
    // target = 6.0 * 8000/1000 = 48 frames/packet; a buggy /8000 would give 6
    // and be rejected as a 87 % deviation.
    ClockController ctrl;
    ctrl.configure(48000, 1000);
    ctrl.setUacVersion(UacVersion::UAC_2_0);

    auto bytes = encodeUac2Feedback(6.0);
    for (int i = 0; i < 100; ++i) {
        ctrl.processFeedback(bytes.data(), 4, UacVersion::UAC_2_0);
    }

    EXPECT_TRUE(ctrl.isStable());
    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 5.0f);
    EXPECT_LT(std::abs(ctrl.getDriftPpm()), 50.0f);
    EXPECT_EQ(ctrl.getFeedbackRejectedCount(), 0u);
}

// ---------------------------------------------------------------------------
// Test 5 — clamp + anti-windup
// ---------------------------------------------------------------------------

TEST(ClockControllerTest, ClampAndAntiWindupBoundsCatchUpBurst) {
    ClockController ctrl;
    ctrl.configure(48000, 1000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    // +9 % feedback: within the ±10 % gate (accepted) but 52.32 frames/packet
    // exceeds nominal+4 (=52), so the per-packet output must be clamped.
    feedUac1(ctrl, 48.0 * 1.09, 200);

    for (int i = 0; i < 100; ++i) {
        const int frames = ctrl.getAdjustedFrameCount(48, 1);
        EXPECT_LE(frames, 52);  // never exceeds nominal + kClockAdjustFramesMax
        EXPECT_GE(frames, 44);
    }

    // Feedback returns to nominal: recovery must not produce a burst longer
    // than ~2 * kClockAdjustFramesMax frames of debt, i.e. a couple of packets.
    feedUac1(ctrl, 48.0, 200);
    int packetsUntilNominal = 0;
    for (int i = 0; i < 50; ++i) {
        const int frames = ctrl.getAdjustedFrameCount(48, 1);
        EXPECT_LE(frames, 52);
        if (frames == 48) { packetsUntilNominal = i; break; }
    }
    EXPECT_LT(packetsUntilNominal, 5);
}

// ---------------------------------------------------------------------------
// Test 6 — rejection of out-of-range feedback
// ---------------------------------------------------------------------------

TEST(ClockControllerTest, OutOfRangeFeedbackRejectedAndTargetUnchanged) {
    ClockController ctrl;
    ctrl.configure(48000, 1000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    // 60 frames/packet ≈ +25 %: well outside ±10 %.
    feedUac1(ctrl, 60.0, 10);

    EXPECT_FALSE(ctrl.isStable());           // target never moved off nominal
    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
    EXPECT_GT(ctrl.getFeedbackRejectedCount(), 0u);
}

// ---------------------------------------------------------------------------
// Implicit-feedback entry point (0.2 wiring) shares the same path
// ---------------------------------------------------------------------------

TEST(ClockControllerTest, MeasuredFramesPerPacketTracksRate) {
    ClockController ctrl;
    ctrl.configure(48000, 1000);

    // +200 PPM measured via implicit feedback: 48.0096 frames/packet.
    for (int i = 0; i < 500; ++i) {
        ctrl.setMeasuredFramesPerPacket(48.0096);
    }

    const long long total = sumAdjusted(ctrl, 48, 10000);
    EXPECT_NEAR(static_cast<double>(total), 48.0096 * 10000.0, 2.0);
}
