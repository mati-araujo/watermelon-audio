// Etapa 2 — Host tests for the extracted USB DSP pacer / trim logic.
//
// These pin the two most delicate decisions of dspThreadFunc (previously inline
// and untestable): the ring-level pacer gate and the combined-excess input trim.

#include <gtest/gtest.h>

#include "../DspPacer.h"

using watermelon_audio::usb::PacerAction;
using watermelon_audio::usb::PacerState;
using watermelon_audio::usb::TrimState;
using watermelon_audio::usb::evaluatePacer;
using watermelon_audio::usb::computeTrimBlocks;

namespace {

// 96-frame (LOW_LATENCY) stereo block: 192 interleaved samples.
constexpr size_t kBlock = 192;

// -------------------------------------------------------------------------
// evaluatePacer — playback-only
// -------------------------------------------------------------------------

TEST(DspPacerGate, PlaybackOnlyProducesWhenRingBelowTarget) {
    PacerState s{};
    s.hasPlayback = true;
    s.hasCapture = false;
    s.outputRingLevel = 500;
    s.outputRingTarget = 1000;
    s.outputSamplesPerBlock = kBlock;
    EXPECT_EQ(PacerAction::PRODUCE, evaluatePacer(s));
}

TEST(DspPacerGate, PlaybackOnlyWaitsWhenRingAtTarget) {
    PacerState s{};
    s.hasPlayback = true;
    s.outputRingLevel = 1000;
    s.outputRingTarget = 1000;
    s.outputSamplesPerBlock = kBlock;
    EXPECT_EQ(PacerAction::WAIT, evaluatePacer(s));
}

// -------------------------------------------------------------------------
// evaluatePacer — capture-only (no playback → output always "ready")
// -------------------------------------------------------------------------

TEST(DspPacerGate, CaptureOnlyProducesWhenInputReady) {
    PacerState s{};
    s.hasPlayback = false;
    s.hasCapture = true;
    s.inputAvailable = kBlock;
    s.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(PacerAction::PRODUCE, evaluatePacer(s));
}

TEST(DspPacerGate, CaptureOnlyWaitsWhenInputShort) {
    PacerState s{};
    s.hasPlayback = false;
    s.hasCapture = true;
    s.inputAvailable = kBlock - 1;
    s.inputSamplesPerBlock = kBlock;
    // No playback → outputCritical can never fire, so a short input waits.
    EXPECT_EQ(PacerAction::WAIT, evaluatePacer(s));
}

// -------------------------------------------------------------------------
// evaluatePacer — duplex
// -------------------------------------------------------------------------

TEST(DspPacerGate, DuplexProducesWhenBothReady) {
    PacerState s{};
    s.hasPlayback = true;
    s.hasCapture = true;
    s.outputRingLevel = 500;
    s.outputRingTarget = 1000;
    s.outputSamplesPerBlock = kBlock;
    s.inputAvailable = kBlock;
    s.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(PacerAction::PRODUCE, evaluatePacer(s));
}

TEST(DspPacerGate, DuplexWaitsWhenInputShortAndNotCritical) {
    PacerState s{};
    s.hasPlayback = true;
    s.hasCapture = true;
    s.outputRingTarget = 1000;               // threshold = max(block, 500) = 500
    s.outputRingLevel = 500;                 // NOT below 500 → not critical
    s.outputSamplesPerBlock = kBlock;
    s.inputAvailable = 0;                    // input not ready
    s.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(PacerAction::WAIT, evaluatePacer(s));
}

TEST(DspPacerGate, DuplexProducesCriticalWhenOutputStarvingAndInputShort) {
    PacerState s{};
    s.hasPlayback = true;
    s.hasCapture = true;
    s.outputRingTarget = 1000;               // threshold = 500
    s.outputRingLevel = 400;                 // below 500 → critical
    s.outputSamplesPerBlock = kBlock;
    s.inputAvailable = 0;                    // input not ready
    s.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(PacerAction::PRODUCE_CRITICAL, evaluatePacer(s));
}

TEST(DspPacerGate, DuplexWaitsWhenRingAtTargetEvenIfInputReady) {
    PacerState s{};
    s.hasPlayback = true;
    s.hasCapture = true;
    s.outputRingLevel = 1000;                // outputReady false dominates
    s.outputRingTarget = 1000;
    s.outputSamplesPerBlock = kBlock;
    s.inputAvailable = kBlock;               // input ready, but ring is full
    s.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(PacerAction::WAIT, evaluatePacer(s));
}

TEST(DspPacerGate, CriticalThresholdIsExclusiveBoundary) {
    PacerState base{};
    base.hasPlayback = true;
    base.hasCapture = true;
    base.outputRingTarget = 1000;            // threshold = max(192, 500) = 500
    base.outputSamplesPerBlock = kBlock;
    base.inputAvailable = 0;                 // input not ready
    base.inputSamplesPerBlock = kBlock;

    PacerState atBoundary = base;
    atBoundary.outputRingLevel = 500;        // NOT < 500 → not critical → WAIT
    EXPECT_EQ(PacerAction::WAIT, evaluatePacer(atBoundary));

    PacerState belowBoundary = base;
    belowBoundary.outputRingLevel = 499;     // < 500 → critical
    EXPECT_EQ(PacerAction::PRODUCE_CRITICAL, evaluatePacer(belowBoundary));
}

TEST(DspPacerGate, CriticalThresholdUsesBlockWhenHalfTargetSmaller) {
    // target/2 = 100 < block(192) → threshold is the block size.
    PacerState s{};
    s.hasPlayback = true;
    s.hasCapture = true;
    s.outputRingTarget = 200;
    s.outputSamplesPerBlock = kBlock;        // 192
    s.inputAvailable = 0;
    s.inputSamplesPerBlock = kBlock;
    s.outputRingLevel = 150;                 // < 192 → critical
    EXPECT_EQ(PacerAction::PRODUCE_CRITICAL, evaluatePacer(s));
}

// -------------------------------------------------------------------------
// computeTrimBlocks — combined-excess law
// -------------------------------------------------------------------------

TEST(DspPacerTrim, TrimsInputExcessInRegime) {
    // Output exactly at target (no deficit), input holds 2 blocks over target.
    TrimState t{};
    t.hasPlayback = true;
    t.inputTarget = 384;                     // 192 frames * 2ch
    t.inputAvailable = 384 + 2 * kBlock;     // +2 blocks
    t.inputChannels = 2;
    t.outputRingLevel = 1000;
    t.outputRingTarget = 1000;               // deficit 0
    t.outputChannels = 2;
    t.framesPerBlock = 96;
    t.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(2, computeTrimBlocks(t));
}

TEST(DspPacerTrim, DoesNotTrimWhenOutputDeficitOffsetsInputExcess) {
    // Post-stall: input backlog is exactly the fuel the output ring needs.
    // Conservation law → excess ~0 → no trim (self-healing preserved).
    TrimState t{};
    t.hasPlayback = true;
    t.inputTarget = 384;
    t.inputAvailable = 384 + 2 * kBlock;     // +192 frames of input excess
    t.inputChannels = 2;
    t.outputRingTarget = 1000;
    t.outputRingLevel = 1000 - 2 * kBlock;   // -192 frames of output deficit
    t.outputChannels = 2;
    t.framesPerBlock = 96;
    t.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(0, computeTrimBlocks(t));
}

TEST(DspPacerTrim, CaptureOnlyTrimsOnInputExcessAlone) {
    TrimState t{};
    t.hasPlayback = false;                    // output term ignored
    t.inputTarget = 384;
    t.inputAvailable = 384 + 2 * kBlock;
    t.inputChannels = 2;
    t.framesPerBlock = 96;
    t.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(2, computeTrimBlocks(t));
}

TEST(DspPacerTrim, NoTrimWhenExcessBelowOneBlock) {
    TrimState t{};
    t.hasPlayback = true;
    t.inputTarget = 384;
    t.inputAvailable = 384 + 100;            // 50 frames excess < 96
    t.inputChannels = 2;
    t.outputRingLevel = 1000;
    t.outputRingTarget = 1000;
    t.outputChannels = 2;
    t.framesPerBlock = 96;
    t.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(0, computeTrimBlocks(t));
}

TEST(DspPacerTrim, AvailabilityCapsTrimBelowComputedExcess) {
    // Huge excess from an output surplus, but only 1 block of input is spendable
    // above the input target → availability, not excess, is the limiter.
    TrimState t{};
    t.hasPlayback = true;
    t.inputTarget = 384;
    t.inputAvailable = 384 + kBlock;         // only 1 block spendable
    t.inputChannels = 2;
    t.outputRingLevel = 2000;                // large surplus
    t.outputRingTarget = 384;
    t.outputChannels = 2;
    t.framesPerBlock = 96;
    t.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(1, computeTrimBlocks(t));
}

TEST(DspPacerTrim, NoTrimWhenInputBelowTarget) {
    TrimState t{};
    t.hasPlayback = false;
    t.inputTarget = 384;
    t.inputAvailable = 200;                  // below target → negative excess
    t.inputChannels = 2;
    t.framesPerBlock = 96;
    t.inputSamplesPerBlock = kBlock;
    EXPECT_EQ(0, computeTrimBlocks(t));
}

TEST(DspPacerTrim, GuardsInvalidBlockSizes) {
    TrimState t{};
    t.hasPlayback = false;
    t.inputTarget = 0;
    t.inputAvailable = 10000;
    t.inputChannels = 2;
    t.inputSamplesPerBlock = kBlock;
    t.framesPerBlock = 0;                    // invalid → guard returns 0
    EXPECT_EQ(0, computeTrimBlocks(t));

    t.framesPerBlock = 96;
    t.inputSamplesPerBlock = 0;              // invalid → guard returns 0
    EXPECT_EQ(0, computeTrimBlocks(t));
}

}  // namespace
