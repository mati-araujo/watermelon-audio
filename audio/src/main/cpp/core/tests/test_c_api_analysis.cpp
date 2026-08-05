/**
 * test_c_api_analysis.cpp
 *
 * Output metering, waveform capture, the modulator and XY automation — the
 * read-out side of the engine, plus the two setters the JNI grouped with it.
 *
 * Everything here was already 1:1 between the two surfaces: no new C API
 * functions and no divergence between them. So the migration was mechanical —
 * but writing the tests turned up something else.
 *
 * THE OUTPUT METERS USED TO BE DEAD, ON BOTH PLATFORMS. AudioEngine read them
 * off OutputNode, whose process() is never called from anywhere — the node is
 * allocated, prepare()d and left alone — so peak and RMS were permanently 0
 * while audio played. NoisyPad's guitar mode polls them for an on-screen level
 * meter, so that meter had never moved. The input meter beside it worked, which
 * is probably why nobody noticed.
 *
 * They now live on OutputStage, the one place the output paths converge
 * (processOutput / processOutputLightweight are each the LAST thing to touch the
 * buffer, once per block). TheOutputMetersFollowTheSignal below is what
 * replaced the characterization test, and it asserts agreement with an
 * independent measurement of the same block rather than mere non-zeroness — a
 * meter reporting a fixed number, or reading the wrong buffer, has to fail it.
 *
 * COVERAGE IS PARTIAL ON PURPOSE: these exercise the main path. The USB direct
 * path (processOutputLightweight) needs the USB backend and is not reachable
 * from a host test, so it is metered by construction and not by assertion.
 *
 * The rest of the metering tests pin what held before and still holds: the
 * -100 dB floor for silence (0 dB would be full scale, the opposite reading),
 * and agreement between the individual getters and the batched one.
 */

#include "support/CApiFixture.h"

#include <gtest/gtest.h>

#include <cmath>

namespace wma_test {
namespace {

constexpr int kBlockFrames = 256;

// The floor both dB getters report for silence. Not 0 — 0 dB is full scale.
constexpr float kSilenceDb = -100.0f;

using CApiAnalysisTest = CApiFixture;

// ===========================================================================
// Output metering
// ===========================================================================

TEST_F(CApiAnalysisTest, TheMetersReadSilenceBeforeAnythingIsRendered) {
    EXPECT_FLOAT_EQ(wma_get_output_peak(mWma, 0), 0.0f);
    EXPECT_FLOAT_EQ(wma_get_output_rms(mWma, 0), 0.0f);

    // The linear getters floor at 0, the dB ones at -100. Reporting 0 in dB
    // would be full scale, which is the opposite of what is happening.
    EXPECT_FLOAT_EQ(wma_get_output_peak_db(mWma, 0), kSilenceDb);
    EXPECT_FLOAT_EQ(wma_get_output_rms_db(mWma, 0), kSilenceDb);
}

TEST_F(CApiAnalysisTest, TheOutputMetersFollowTheSignal) {
    // Replaces TheOutputMetersStayAtZeroEvenWithAudioPlaying, which pinned the
    // broken behaviour on purpose.
    //
    // The assertion is AGREEMENT, not non-zeroness. renderBlockPeak() renders a
    // block through onAudioReady and measures the resulting output buffer
    // itself — and that is the very call that updates the meter, so the two are
    // measuring the same samples by different routes. A meter that returned a
    // fixed number, or metered the wrong buffer, or metered before the limiter,
    // would sail through an EXPECT_GT(.., 0) and has to fail this.
    startAt(48000, 0);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(20, kBlockFrames);  // let the start fade and the limiter settle

    const float blockPeak = renderBlockPeak(kBlockFrames);
    ASSERT_GT(blockPeak, 0.01f) << "no signal — the rest of this test proves nothing";

    // Steady tone, so the decaying hold and the current block agree closely.
    EXPECT_NEAR(wma_get_output_peak(mWma, 0), blockPeak, 0.05f);
    EXPECT_NEAR(wma_get_output_peak(mWma, 1), blockPeak, 0.05f);
}

TEST_F(CApiAnalysisTest, TheRmsConvergesOnTheRmsOfTheSignal) {
    // This is the one that pins the RMS integration time, and it exists because
    // the smoothing was wrong in the code the meters came from: the coefficient
    // is per SAMPLE and OutputNode applied it once per BLOCK, which at 48 kHz
    // and 256-frame blocks turned a 300 ms average into a ~77 SECOND one. The
    // RMS would sit near zero for a minute. Nothing caught it because the whole
    // meter was dead.
    //
    // A sine's RMS is its peak over sqrt(2). After ~1 s with a 300 ms time
    // constant the average is >95% converged, so the target is reachable — but
    // only if the coefficient is raised to the block size. With the old
    // per-block application this reads ~1% of the target and fails loudly.
    startAt(48000, 0);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(200, kBlockFrames);  // ~1.07 s at 48 kHz

    const float peak = wma_get_output_peak(mWma, 0);
    ASSERT_GT(peak, 0.01f);

    const float expectedRms = peak / std::sqrt(2.0f);
    EXPECT_NEAR(wma_get_output_rms(mWma, 0), expectedRms, 0.1f);
}

TEST_F(CApiAnalysisTest, ThePeakDecaysOnceTheSignalStops) {
    // The peak is a decaying hold, not an instantaneous reading — a transient
    // has to stay visible long enough to see. So it must come down on silence,
    // and it must not come down instantly.
    startAt(48000, 0);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(20, kBlockFrames);

    const float loud = wma_get_output_peak(mWma, 0);
    ASSERT_GT(loud, 0.01f);

    wma_set_frequency_amplitude(mWma, 440.0f, 0.0f);
    render(2, kBlockFrames);
    const float justAfter = wma_get_output_peak(mWma, 0);
    EXPECT_LT(justAfter, loud) << "the hold never decays — a transient would stick forever";
    EXPECT_GT(justAfter, 0.0f) << "the hold collapsed in two blocks — nothing to see on screen";

    render(400, kBlockFrames);  // ~2 s of silence
    EXPECT_LT(wma_get_output_peak(mWma, 0), loud * 0.1f);
}

TEST_F(CApiAnalysisTest, TheDbGettersAgreeWithTheLinearOnesWhateverTheyRead) {
    // Independent of the bug above: whatever the linear getter reports, the dB
    // one has to be its log, and the floor has to be -100 rather than -inf.
    startAt(48000, 0);
    render(4, kBlockFrames);

    const float peak = wma_get_output_peak(mWma, 0);
    if (peak > 0.0f) {
        EXPECT_NEAR(wma_get_output_peak_db(mWma, 0), 20.0f * std::log10(peak), 0.01f);
    } else {
        EXPECT_FLOAT_EQ(wma_get_output_peak_db(mWma, 0), kSilenceDb);
    }

    const float rms = wma_get_output_rms(mWma, 0);
    if (rms > 0.0f) {
        EXPECT_NEAR(wma_get_output_rms_db(mWma, 0), 20.0f * std::log10(rms), 0.01f);
    } else {
        EXPECT_FLOAT_EQ(wma_get_output_rms_db(mWma, 0), kSilenceDb);
    }
}

TEST_F(CApiAnalysisTest, TheBatchedLevelsMatchTheIndividualGetters) {
    startAt(48000, 0);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(20, kBlockFrames);

    // Zeros today (see above), but the point is agreement between the two
    // read-out paths, which holds either way and would catch a transposition
    // the moment the meters start moving.
    // [peakL, peakR, rmsL, rmsR] — the order nativeGetOutputLevels packs into
    // its float[4], and the one place a transposition would hide.
    float levels[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    wma_get_output_levels(mWma, levels);

    EXPECT_FLOAT_EQ(levels[0], wma_get_output_peak(mWma, 0));
    EXPECT_FLOAT_EQ(levels[1], wma_get_output_peak(mWma, 1));
    EXPECT_FLOAT_EQ(levels[2], wma_get_output_rms(mWma, 0));
    EXPECT_FLOAT_EQ(levels[3], wma_get_output_rms(mWma, 1));
}

TEST_F(CApiAnalysisTest, TheBatchedLevelsLeaveTheBufferAloneWithNoEngine) {
    // nativeGetOutputLevels pre-zeroes its array precisely because this leaves
    // the buffer untouched — it answers with zeros rather than null, and that
    // only works if the zeros are already there.
    float levels[4] = {7.0f, 7.0f, 7.0f, 7.0f};
    wma_get_output_levels(nullptr, levels);

    for (float v : levels) {
        EXPECT_FLOAT_EQ(v, 7.0f);
    }
}

// ===========================================================================
// Waveform capture
// ===========================================================================

TEST_F(CApiAnalysisTest, TheWaveformCaptureFillsNoMoreThanItIsGiven) {
    startAt(48000, 0);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(20, kBlockFrames);

    constexpr int kCapacity = 64;
    float buffer[kCapacity];
    for (float& v : buffer) v = 12345.0f;

    const int written = wma_get_waveform_samples(mWma, buffer, kCapacity);

    EXPECT_GE(written, 0);
    EXPECT_LE(written, kCapacity)
        << "writing past what the caller asked for is how the JNI overruns a "
           "Java array";
}

TEST_F(CApiAnalysisTest, TheWaveformCaptureRejectsDegenerateRequests) {
    startAt(48000, 0);
    render(4, kBlockFrames);

    float buffer[8];
    EXPECT_EQ(wma_get_waveform_samples(mWma, nullptr, 8), 0);
    EXPECT_EQ(wma_get_waveform_samples(mWma, buffer, 0), 0);
    EXPECT_EQ(wma_get_waveform_samples(mWma, buffer, -1), 0);
}

// ===========================================================================
// Modulator — the one part of this category with error codes
// ===========================================================================

TEST_F(CApiAnalysisTest, TheModulatorTypeRangeIsEnforced) {
    EXPECT_EQ(wma_set_modulator_type(mWma, 0), WMA_OK);
    EXPECT_EQ(wma_set_modulator_type(mWma, 7), WMA_OK);

    // Valid types are 0–7; the JNI used to check this and now forwards it.
    EXPECT_EQ(wma_set_modulator_type(mWma, 8), WMA_ERROR_INVALID_PARAMETER_ID);
    EXPECT_EQ(wma_set_modulator_type(mWma, -1), WMA_ERROR_INVALID_PARAMETER_ID);
}

TEST_F(CApiAnalysisTest, TheModulatorRejectsBadParameterIdsAndNonFiniteValues) {
    EXPECT_EQ(wma_set_modulator_param(mWma, 0, 0.5f), WMA_OK);

    EXPECT_EQ(wma_set_modulator_param(mWma, -1, 0.5f), WMA_ERROR_INVALID_PARAMETER_ID);

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(wma_set_modulator_param(mWma, 0, inf), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_set_modulator_param(mWma, 0, nan), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
}

// ===========================================================================
// Automation
// ===========================================================================

TEST_F(CApiAnalysisTest, AutomatingAnEffectThatIsNotThereIsIgnored) {
    ASSERT_EQ(wma_effect_chain_size(mWma), 0);

    // Out of range for an empty chain — must not index into it.
    wma_set_automation_param(mWma, 0, 0, 0.5f);
    wma_set_automation_param(mWma, 5, 0, 0.5f);
    wma_set_automation_param(mWma, -1, 0, 0.5f);

    EXPECT_TRUE(wma_is_initialized(mWma));
}

TEST_F(CApiAnalysisTest, AutomationWritesThroughToTheEffectParameter) {
    ASSERT_EQ(wma_effect_add(mWma, 0), 0);  // FILTER

    // The XY value arrives already normalised, so this is the same write the
    // plain parameter setter does — which is exactly what the JNI comment says
    // and what makes the two paths agree.
    wma_set_automation_param(mWma, 0, 0, 0.5f);
    const float viaAutomation = wma_effect_get_param(mWma, 0, 0);

    wma_effect_set_param(mWma, 0, 0, 0.5f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), viaAutomation);
}

TEST_F(CApiAnalysisTest, TheAutomationAxisRangeIsEnforced) {
    // Three axes: 0, 1, 2. Anything else is dropped rather than indexed.
    wma_apply_automation(mWma, 0, 0.5f);
    wma_apply_automation(mWma, 2, 0.5f);
    wma_apply_automation(mWma, 3, 0.5f);
    wma_apply_automation(mWma, -1, 0.5f);

    // And the value is clamped, so an out-of-range one is not passed through.
    wma_apply_automation(mWma, 0, 5.0f);
    wma_apply_automation(mWma, 0, -5.0f);

    EXPECT_TRUE(wma_is_initialized(mWma));
}

// ===========================================================================
// Null handle
// ===========================================================================

TEST(CApiAnalysisNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    EXPECT_FLOAT_EQ(wma_get_output_peak(nullptr, 0), 0.0f);
    EXPECT_FLOAT_EQ(wma_get_output_rms(nullptr, 0), 0.0f);
    EXPECT_FLOAT_EQ(wma_get_output_peak_db(nullptr, 0), kSilenceDb);
    EXPECT_FLOAT_EQ(wma_get_output_rms_db(nullptr, 0), kSilenceDb);

    float buffer[8];
    EXPECT_EQ(wma_get_waveform_samples(nullptr, buffer, 8), 0);

    EXPECT_EQ(wma_set_modulator_type(nullptr, 1), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_set_modulator_param(nullptr, 0, 0.5f), WMA_ERROR_NOT_INITIALIZED);
}

TEST(CApiAnalysisNullHandle, EveryMutatorIsANoOpRatherThanACrash) {
    wma_set_automation_param(nullptr, 0, 0, 0.5f);
    wma_apply_automation(nullptr, 0, 0.5f);
    wma_get_output_levels(nullptr, nullptr);
    SUCCEED();
}

}  // namespace
}  // namespace wma_test
