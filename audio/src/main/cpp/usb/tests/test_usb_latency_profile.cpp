// Fase 1 — Latency profiles: 1 ms iso transfers and the ms-based pacer math.

#include <gtest/gtest.h>

#include "../LatencyProfile.h"
#include "../UsbIsoTiming.h"

using namespace watermelon_audio::usb;

// ============================================================================
// calculateIsoTransferTiming with targetTransferMs=1 (L1)
// ============================================================================

TEST(LatencyProfileTiming, FullSpeedOneMsIsOnePacketPerTransfer) {
    // FS: 1000 packets/s, bInterval=1 → 1 ms == 1 packet/URB.
    const auto t = calculateIsoTransferTiming(
        48000, /*highSpeed=*/false, /*endpointInterval=*/1, /*targetTransferMs=*/1);
    EXPECT_EQ(t.packetsPerSecond, 1000);
    EXPECT_EQ(t.framesPerPacket, 48);
    EXPECT_EQ(t.packetsPerTransfer, 1);
}

TEST(LatencyProfileTiming, HighSpeedOneMsIsEightPacketsPerTransfer) {
    // HS bInterval=1: 8000 packets/s → 1 ms == 8 microframe packets/URB.
    const auto t = calculateIsoTransferTiming(
        48000, /*highSpeed=*/true, /*endpointInterval=*/1, /*targetTransferMs=*/1);
    EXPECT_EQ(t.packetsPerSecond, 8000);
    EXPECT_EQ(t.framesPerPacket, 6);
    EXPECT_EQ(t.packetsPerTransfer, 8);
}

TEST(LatencyProfileTiming, HighSpeedBInterval4OneMsIsOnePacketPerTransfer) {
    // HS bInterval=4: serviced once per 1 ms (8 microframe slots) → 1000 pkt/s,
    // 1 ms == 1 packet/URB. The hardware service cadence is the latency floor.
    const auto t = calculateIsoTransferTiming(
        48000, /*highSpeed=*/true, /*endpointInterval=*/4, /*targetTransferMs=*/1);
    EXPECT_EQ(t.packetsPerSecond, 1000);
    EXPECT_EQ(t.framesPerPacket, 48);
    EXPECT_EQ(t.packetsPerTransfer, 1);
}

// ============================================================================
// outputRingTargetSamples — SAFE is bit-identical to the historical target
// ============================================================================

TEST(LatencyProfilePacer, SafeTargetMatchesHistoricalHighSpeed) {
    // HS SAFE: 8 ms transfer = 64 packets * 6 frames = 384 frames.
    // Historical formula: (numTransfers+1) transfers = 4 * 384 = 1536 frames.
    const auto tuning = UsbLatencyTuning::safe();
    const size_t target = outputRingTargetSamples(
        /*packetsPerTransfer=*/64, /*framesPerPacket=*/6,
        tuning.jitterBudgetMs, /*sampleRate=*/48000, /*channelCount=*/1);
    EXPECT_EQ(target, 1536u);  // 384 + 24ms*48 = 384 + 1152
}

TEST(LatencyProfilePacer, SafeTargetMatchesHistoricalFullSpeed) {
    // FS SAFE: 8 ms transfer = 8 packets * 48 frames = 384 frames. Same target.
    const auto tuning = UsbLatencyTuning::safe();
    const size_t target = outputRingTargetSamples(
        8, 48, tuning.jitterBudgetMs, 48000, /*channelCount=*/2);
    EXPECT_EQ(target, 1536u * 2);
}

TEST(LatencyProfilePacer, LowLatencyTargetIsFiveMs) {
    // LOW_LATENCY HS: 1 ms transfer (8*6=48 frames) + 4 ms jitter (192 frames)
    // = 240 frames = 5 ms @ 48k.
    const auto tuning = UsbLatencyTuning::lowLatency();
    const size_t target = outputRingTargetSamples(
        /*packetsPerTransfer=*/8, /*framesPerPacket=*/6,
        tuning.jitterBudgetMs, 48000, /*channelCount=*/1);
    EXPECT_EQ(target, 240u);  // 48 + 4*48
}

TEST(LatencyProfilePacer, LowLatencyTargetAt44100) {
    // 44.1 kHz truncates jitter frames: 4 * 44100 / 1000 = 176.
    const size_t target = outputRingTargetSamples(
        8, 5 /*44100/8000=5*/, /*jitterBudgetMs=*/4, 44100, 1);
    EXPECT_EQ(target, static_cast<size_t>(40 + 176));
}

// ============================================================================
// outputPrefillSamples — ring lands exactly at target after the initial fills
// ============================================================================

TEST(LatencyProfilePrefill, PrefillLeavesRingAtTargetSafe) {
    const auto tuning = UsbLatencyTuning::safe();  // numTransfers=3
    const size_t target = outputRingTargetSamples(64, 6, tuning.jitterBudgetMs, 48000, 1);
    const size_t prefill = outputPrefillSamples(tuning.numTransfers, 64, 6, 1, target);
    // After numTransfers initial fills drain inflight, ring == target.
    const size_t inflight = static_cast<size_t>(tuning.numTransfers) * 64 * 6 * 1;
    EXPECT_EQ(prefill - inflight, target);
}

TEST(LatencyProfilePrefill, PrefillLeavesRingAtTargetLowLatency) {
    const auto tuning = UsbLatencyTuning::lowLatency();  // numTransfers=4
    const size_t target = outputRingTargetSamples(8, 6, tuning.jitterBudgetMs, 48000, 2);
    const size_t prefill = outputPrefillSamples(tuning.numTransfers, 8, 6, 2, target);
    const size_t inflight = static_cast<size_t>(tuning.numTransfers) * 8 * 6 * 2;
    EXPECT_EQ(prefill - inflight, target);
}

// ============================================================================
// Presets sanity
// ============================================================================

TEST(LatencyProfilePresets, SafeIsCurrentBehavior) {
    const auto s = UsbLatencyTuning::safe();
    EXPECT_EQ(s.targetTransferMs, 8);
    EXPECT_EQ(s.numTransfers, 3);
    EXPECT_EQ(s.jitterBudgetMs, 24);
    EXPECT_EQ(s.dspBlockFrames, 256);
    EXPECT_EQ(s.ringCapacityMs, 100);
}

TEST(LatencyProfilePresets, LowLatencyKnobs) {
    const auto l = UsbLatencyTuning::lowLatency();
    EXPECT_EQ(l.targetTransferMs, 1);
    EXPECT_EQ(l.numTransfers, 4);
    EXPECT_EQ(l.jitterBudgetMs, 4);
    EXPECT_EQ(l.dspBlockFrames, 96);
    EXPECT_EQ(l.ringCapacityMs, 50);
}

TEST(LatencyProfilePresets, ForProfileDispatch) {
    EXPECT_EQ(UsbLatencyTuning::forProfile(UsbLatencyProfile::SAFE).targetTransferMs, 8);
    EXPECT_EQ(UsbLatencyTuning::forProfile(UsbLatencyProfile::LOW_LATENCY).targetTransferMs, 1);
}
