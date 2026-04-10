// Stage 1 — ClockController unit tests
//
// Verifies that:
//  1. UAC 1.0 feedback (3-byte, 10.14 fixed point) parses to the right
//     instantaneous rate and reports near-zero drift on a steady source.
//  2. UAC 2.0 feedback (4-byte, 16.16 fixed point) parses to the right
//     instantaneous rate and reports near-zero drift on a steady source.
//  3. The PID converges (residual under tolerance) when fed feedback that
//     simulates a device running consistently fast by ~100 PPM.

#include <gtest/gtest.h>

#include "../../backends/ClockController.h"

#include <array>
#include <cmath>
#include <cstdint>

using watermelon_audio::ClockController;
using watermelon_audio::UacVersion;

namespace {

// Encode a 10.14 fixed-point feedback value (UAC 1.0) into 3 little-endian
// bytes. `samplesPerFrame` is the floating-point number of audio samples
// the device is producing per 1 ms USB frame (e.g. 48.0 for 48 kHz).
std::array<uint8_t, 3> encodeUac1Feedback(float samplesPerFrame) {
    const uint32_t raw = static_cast<uint32_t>(
        std::lround(static_cast<double>(samplesPerFrame) * 16384.0));  // 2^14
    return {
        static_cast<uint8_t>(raw & 0xff),
        static_cast<uint8_t>((raw >> 8) & 0xff),
        static_cast<uint8_t>((raw >> 16) & 0xff),
    };
}

// Encode a 16.16 fixed-point feedback value (UAC 2.0) into 4 little-endian
// bytes. `samplesPerMicroframe` is the floating-point number of audio
// samples the device is producing per 125 µs USB microframe (e.g. 6.0 for
// 48 kHz).
std::array<uint8_t, 4> encodeUac2Feedback(float samplesPerMicroframe) {
    const uint32_t raw = static_cast<uint32_t>(
        std::lround(static_cast<double>(samplesPerMicroframe) * 65536.0));  // 2^16
    return {
        static_cast<uint8_t>(raw & 0xff),
        static_cast<uint8_t>((raw >> 8) & 0xff),
        static_cast<uint8_t>((raw >> 16) & 0xff),
        static_cast<uint8_t>((raw >> 24) & 0xff),
    };
}

}  // namespace

TEST(ClockControllerTest, Uac1FeedbackParsedAsThreeBytes) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    // 48.0 samples per 1 ms frame at 48 kHz: a perfectly steady device.
    // Push enough samples to fill the moving-average buffer so the PID
    // sees a stable measurement.
    auto bytes = encodeUac1Feedback(48.0f);
    for (int i = 0; i < 32; ++i) {
        ctrl.processFeedback(bytes.data(), 3, UacVersion::UAC_1_0);
    }

    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
    EXPECT_LT(std::abs(ctrl.getDriftPpm()), 50.0f);
    EXPECT_TRUE(ctrl.isStable());
}

TEST(ClockControllerTest, Uac2FeedbackParsedAsFourBytes) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_2_0);

    // 6.0 samples per 125 µs microframe at 48 kHz.
    auto bytes = encodeUac2Feedback(6.0f);
    for (int i = 0; i < 32; ++i) {
        ctrl.processFeedback(bytes.data(), 4, UacVersion::UAC_2_0);
    }

    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
    EXPECT_LT(std::abs(ctrl.getDriftPpm()), 50.0f);
    EXPECT_TRUE(ctrl.isStable());
}

TEST(ClockControllerTest, Uac1FeedbackThreeBytesIsNotMisparsedAsUac2) {
    // Regression: previously, the transfer manager inferred the wire format
    // from packet length (length>=4 → UAC2). A UAC 1.0 device that emitted
    // 3-byte feedback was correctly classified, but if the call site mixes
    // up the version we want the controller to still produce a sane result
    // when given the right version explicitly.
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    auto bytes = encodeUac1Feedback(48.0f);
    for (int i = 0; i < 32; ++i) {
        ctrl.processFeedback(bytes.data(), 3, UacVersion::UAC_1_0);
    }

    // If the parsing path swapped UAC1↔UAC2 it would divide the raw value
    // by 65536 instead of 16384, yielding a measured rate ~4× too small.
    EXPECT_GT(ctrl.getCurrentSampleRate(), 47000.0f);
    EXPECT_LT(ctrl.getCurrentSampleRate(), 49000.0f);
}

TEST(ClockControllerTest, DriftConvergesUnderSimulated100PPMFast) {
    // Simulate a device running 100 PPM fast: 48000 * (1 + 100e-6) = 48004.8
    // → samples/frame = 48.0048. Push 1000 feedback iterations and expect
    // the controller to converge close to that rate (within 10 PPM residual).
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    constexpr float samplesPerFrame = 48.0f * (1.0f + 100.0e-6f);
    auto bytes = encodeUac1Feedback(samplesPerFrame);
    for (int i = 0; i < 1000; ++i) {
        ctrl.processFeedback(bytes.data(), 3, UacVersion::UAC_1_0);
    }

    // Measured rate should be ≈ 48004.8 Hz; reported drift should sit around
    // +100 PPM. Allow ± 15 PPM of slack for moving-average smoothing.
    EXPECT_NEAR(ctrl.getDriftPpm(), 100.0f, 15.0f);
    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48004.8f, 5.0f);
}

TEST(ClockControllerTest, SetUacVersionResetsStateOnChange) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    auto bytes = encodeUac1Feedback(48.5f);
    for (int i = 0; i < 16; ++i) {
        ctrl.processFeedback(bytes.data(), 3, UacVersion::UAC_1_0);
    }
    const float driftBefore = ctrl.getDriftPpm();
    EXPECT_GT(std::abs(driftBefore), 100.0f);  // ~10 000 PPM, definitely non-zero

    // Switching version resets PID + buffers. After reset and before any
    // new feedback the reported rate should be back to nominal.
    ctrl.setUacVersion(UacVersion::UAC_2_0);
    EXPECT_FALSE(ctrl.isStable());
}

TEST(ClockControllerTest, RejectsTooShortPayload) {
    ClockController ctrl(48000);
    ctrl.setUacVersion(UacVersion::UAC_1_0);

    // Length < 3 must not corrupt internal state — controller should keep
    // its initial nominal rate.
    uint8_t bogus[2] = {0xff, 0xff};
    ctrl.processFeedback(bogus, 2, UacVersion::UAC_1_0);
    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
}
