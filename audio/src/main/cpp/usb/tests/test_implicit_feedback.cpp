// Fase 0.2 — Implicit feedback estimator tests (hallazgo C2)
//
//  1. The estimator only emits once a full window of service intervals has
//     elapsed, and the value is frames-per-input-packet.
//  2. Zero-frame packets still count as elapsed intervals (rate information).
//  3. Driving the estimator output into the ClockController converges the
//     adjusted frame count to the simulated rate (same criterion as 0.1 test 1).

#include <gtest/gtest.h>

#include "../ImplicitFeedbackEstimator.h"
#include "../../backends/ClockController.h"

#include <optional>

using watermelon_audio::ClockController;
using watermelon_audio::usb::ImplicitFeedbackEstimator;

namespace {

long long sumAdjusted(ClockController& ctrl, int nominalFrames, int packets) {
    long long total = 0;
    for (int i = 0; i < packets; ++i) {
        total += ctrl.getAdjustedFrameCount(nominalFrames, 1);
    }
    return total;
}

}  // namespace

TEST(ImplicitFeedbackEstimatorTest, NoOutputBeforeWindowCloses) {
    ImplicitFeedbackEstimator est;
    // Feed window-1 packets of 48 frames each → no output yet.
    for (uint64_t p = 0; p < ImplicitFeedbackEstimator::kWindowPackets - 1; ++p) {
        EXPECT_FALSE(est.onPackets(48, 1).has_value());
    }
    auto closed = est.onPackets(48, 1);
    ASSERT_TRUE(closed.has_value());
    EXPECT_NEAR(*closed, 48.0, 1e-9);
}

TEST(ImplicitFeedbackEstimatorTest, ZeroLengthPacketsCountAsIntervals) {
    ImplicitFeedbackEstimator est;
    // Half the window delivers 48 frames, the other half delivers 0 frames
    // (e.g. a momentarily silent capture). Both halves are elapsed intervals,
    // so the measured rate is the average: 24 frames/packet.
    const uint64_t half = ImplicitFeedbackEstimator::kWindowPackets / 2;
    std::optional<double> result;
    for (uint64_t p = 0; p < half; ++p) est.onPackets(48, 1);
    for (uint64_t p = 0; p < half; ++p) result = est.onPackets(0, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(*result, 24.0, 1e-9);
}

TEST(ImplicitFeedbackEstimatorTest, BatchedPacketsAggregateLikeSinglePackets) {
    // A transfer carrying many packets at once must accumulate identically to
    // feeding them one at a time.
    ImplicitFeedbackEstimator est;
    auto r = est.onPackets(48 * ImplicitFeedbackEstimator::kWindowPackets,
                           ImplicitFeedbackEstimator::kWindowPackets);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 48.0, 1e-9);
}

TEST(ImplicitFeedbackEstimatorTest, FeedsClockControllerToSimulatedRate) {
    // Simulate a device running +200 PPM: 48.0096 frames per input packet.
    // Run several windows through the estimator → ClockController; the
    // adjusted output frame count must follow the simulated rate.
    ClockController ctrl;
    ctrl.configure(48000, 1000);

    ImplicitFeedbackEstimator est;
    constexpr double framesPerPacket = 48.0096;
    // Model the device's own integer emission: a fractional accumulator that
    // delivers whole frames each window. Over many windows the per-window
    // integer counts straddle the true fractional rate, and the EMA averages
    // the quantization away — mirroring real hardware (spec §0.2 resolution).
    double deviceAccum = 0.0;
    for (int w = 0; w < 200; ++w) {
        const uint64_t pkts = ImplicitFeedbackEstimator::kWindowPackets;
        deviceAccum += framesPerPacket * static_cast<double>(pkts);
        const uint64_t frames = static_cast<uint64_t>(deviceAccum);  // floor
        deviceAccum -= static_cast<double>(frames);
        auto out = est.onPackets(frames, pkts);
        if (out.has_value()) {
            ctrl.setMeasuredFramesPerPacket(*out);
        }
    }

    EXPECT_TRUE(ctrl.isStable());
    const long long total = sumAdjusted(ctrl, 48, 10000);
    EXPECT_NEAR(static_cast<double>(total), framesPerPacket * 10000.0, 5.0);
}
