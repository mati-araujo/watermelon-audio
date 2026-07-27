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
 * THE OUTPUT METERS DO NOT WORK, ON EITHER PLATFORM. AudioEngine reads them off
 * OutputNode, and OutputNode::process() is never called from anywhere; the node
 * is allocated, prepare()d, and left alone. So peak and RMS are permanently 0
 * while audio plays. NoisyPad's guitar mode polls them for an on-screen level
 * meter, which means that meter has never moved.
 *
 * That is documented by TheOutputMetersStayAtZeroEvenWithAudioPlaying below,
 * which asserts the wrong-looking thing on purpose and fails the day someone
 * fixes it. The rest of the metering tests pin what still holds regardless: the
 * -100 dB floor for silence (0 dB would be full scale, the opposite reading),
 * and agreement between the individual getters and the batched one.
 */

#include "support/CApiFixture.h"

#include <gtest/gtest.h>

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

TEST_F(CApiAnalysisTest, TheOutputMetersStayAtZeroEvenWithAudioPlaying) {
    // NOT the behaviour these should have, and this one is visible to users.
    //
    // AudioEngine::getOutputPeakLevel reads OutputNode, and OutputNode::process()
    // is NEVER CALLED — not from AudioEngine, not from the AudioGraph, not from
    // anywhere. The node is allocated and prepare()d and then left alone, so its
    // peak/RMS atomics keep their initial 0.
    //
    // NoisyPad's guitar mode polls getOutputLevels() in a loop and feeds the
    // result to an output level meter (GuitarModeViewModel.startMetering). That
    // meter has never moved. The input meter beside it works, because InputNode
    // IS driven — which is probably why nobody noticed.
    //
    // The assertion below is deliberately the wrong-looking one: audio IS
    // playing, and the meters still read silence. Registered as its own item; if
    // someone wires OutputNode into the render path, this test fails and that is
    // how they will find out to update it.
    startAt(48000, 0);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(20, kBlockFrames);

    // There IS signal — the same render that leaves these meters at zero.
    ASSERT_GT(renderBlockPeak(kBlockFrames), 0.01f);

    EXPECT_FLOAT_EQ(wma_get_output_peak(mWma, 0), 0.0f)
        << "if this is now non-zero, OutputNode got wired up — update this test";
    EXPECT_FLOAT_EQ(wma_get_output_rms(mWma, 0), 0.0f);
    EXPECT_FLOAT_EQ(wma_get_output_peak_db(mWma, 0), kSilenceDb);
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
