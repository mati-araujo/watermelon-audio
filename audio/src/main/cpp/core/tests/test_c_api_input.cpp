/**
 * test_c_api_input.cpp
 *
 * The input / monitoring surface of the C API (section 12 of
 * watermelon_audio.h), which WA-2.6 turned into the only implementation: the 20
 * matching JNI entry points are now one-line calls into these functions.
 *
 * WHAT THIS SUITE CAN AND CANNOT SEE.
 *
 * core/tests substitutes nodes/InputNode.cpp with a link-time stub that has no
 * behaviour (support/test_input_node_stub.cpp). So there is no point asserting
 * anything about levels, gating or latency — those numbers come from the stub.
 *
 * What is worth asserting, and what these tests are about:
 *
 *   1. The no-node contract. Almost every wma_input_* has to answer sensibly
 *      before an InputNode exists, because that is the state the JNI used to
 *      handle with its own `if (!g_jniState.inputNode) return <default>;` and
 *      no longer does. Two distinct cases: a null handle, and a live engine
 *      that simply has no input node yet — the second is the common one.
 *
 *   2. That each function reaches the right node method. The stub does store
 *      source, gain, the noise-gate flag, monitoring and monitoring volume, so
 *      a set/get round-trip proves the wiring — that wma_input_set_gain does
 *      not land on the noise gate, for instance.
 *
 *   3. The metering snapshot's shape and its "no data" answer, which the JNI
 *      relies on to return null to Kotlin rather than an array of zeros.
 *
 * The level getter is the useful probe for (1) vs (2): the no-node answer is
 * -100 dB and the stub's is -120, so the two states are actually distinguishable
 * instead of both reading as "silence".
 *
 * NOT tested here, deliberately: the monitoring volume clamp. InputNode itself
 * clamps, in production and in the stub, so an assertion through the node would
 * pass whether or not wma_input_set_monitoring_volume clamps at all. The clamp
 * stays as defence in depth; this suite does not pretend to cover it.
 */

#include "support/CApiFixture.h"

#include <gtest/gtest.h>

namespace wma_test {
namespace {

// InputNode::getInputLevel with no node vs. with the stub attached. The gap
// between them is what makes the no-node assertions mean something.
constexpr float kNoNodeLevelDb = -100.0f;
constexpr float kStubLevelDb = -120.0f;

using CApiInputTest = CApiFixture;

/// Create the input node the same way wma_input_start() does, minus the stream.
void attachInputNode(WmaEngine* engine) {
    ASSERT_TRUE(wmaEnsureInputNode(engine));
}

// ===========================================================================
// 1a. No input node yet — the state the JNI used to guard by hand
// ===========================================================================

TEST_F(CApiInputTest, EveryQueryAnswersBeforeAnInputNodeExists) {
    ASSERT_EQ(mWma->inputNode, nullptr) << "a fresh engine must not have one yet";

    EXPECT_FALSE(wma_input_is_running(mWma));
    EXPECT_EQ(wma_input_get_source(mWma), 0);
    EXPECT_FLOAT_EQ(wma_input_get_gain(mWma), 0.0f);
    EXPECT_FALSE(wma_input_is_noise_gate_enabled(mWma));
    EXPECT_FALSE(wma_input_is_noise_gate_open(mWma));
    EXPECT_FALSE(wma_input_is_clipping(mWma));
    EXPECT_FLOAT_EQ(wma_input_get_latency_ms(mWma), 0.0f);
    EXPECT_FALSE(wma_input_is_monitoring_enabled(mWma));
    EXPECT_FLOAT_EQ(wma_input_get_monitoring_volume(mWma), 0.0f);

    // -100 dB, not 0: with no input there is no signal, and 0 dB is full scale.
    EXPECT_FLOAT_EQ(wma_input_get_level(mWma, 0), kNoNodeLevelDb);
    EXPECT_FLOAT_EQ(wma_input_get_level_linear(mWma, 0), 0.0f);
}

TEST_F(CApiInputTest, EverySetterIsANoOpBeforeAnInputNodeExists) {
    // None of these may create a node as a side effect — only wma_input_start()
    // is allowed to do that.
    wma_input_set_source(mWma, 1);
    wma_input_set_gain(mWma, -6.0f);
    wma_input_set_noise_gate(mWma, true);
    wma_input_set_noise_gate_threshold(mWma, -40.0f);
    wma_input_set_monitoring(mWma, true);
    wma_input_set_monitoring_volume(mWma, 0.5f);
    wma_input_stop(mWma);
    wma_input_release(mWma);

    EXPECT_EQ(mWma->inputNode, nullptr);
    EXPECT_EQ(wma_input_get_source(mWma), 0);
    EXPECT_FALSE(wma_input_is_noise_gate_enabled(mWma));
}

// ===========================================================================
// 1b. Null handle
// ===========================================================================

TEST(CApiInputNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    EXPECT_FALSE(wma_input_is_running(nullptr));
    EXPECT_EQ(wma_input_get_source(nullptr), 0);
    EXPECT_FLOAT_EQ(wma_input_get_gain(nullptr), 0.0f);
    EXPECT_FALSE(wma_input_is_noise_gate_enabled(nullptr));
    EXPECT_FALSE(wma_input_is_noise_gate_open(nullptr));
    EXPECT_FLOAT_EQ(wma_input_get_level(nullptr, 0), kNoNodeLevelDb);
    EXPECT_FLOAT_EQ(wma_input_get_level_linear(nullptr, 0), 0.0f);
    EXPECT_FALSE(wma_input_is_clipping(nullptr));
    EXPECT_FLOAT_EQ(wma_input_get_latency_ms(nullptr), 0.0f);
    EXPECT_FALSE(wma_input_is_monitoring_enabled(nullptr));
    EXPECT_FLOAT_EQ(wma_input_get_monitoring_volume(nullptr), 0.0f);
    EXPECT_FALSE(wma_input_start(nullptr));
}

TEST(CApiInputNullHandle, EveryMutatorIsANoOpRatherThanACrash) {
    wma_input_set_source(nullptr, 1);
    wma_input_set_gain(nullptr, -6.0f);
    wma_input_set_noise_gate(nullptr, true);
    wma_input_set_noise_gate_threshold(nullptr, -40.0f);
    wma_input_set_monitoring(nullptr, true);
    wma_input_set_monitoring_volume(nullptr, 0.5f);
    wma_input_stop(nullptr);
    wma_input_release(nullptr);
    SUCCEED();
}

// ===========================================================================
// 2. Wiring: each function reaches the node method it claims to
// ===========================================================================

TEST_F(CApiInputTest, TheGettersReachTheNodeOnceItExists) {
    attachInputNode(mWma);

    // The point of this assertion is the -100 / -120 difference: it proves the
    // call actually went to the node, rather than the no-node default happening
    // to look like a plausible silence reading.
    EXPECT_FLOAT_EQ(wma_input_get_level(mWma, 0), kStubLevelDb);
    EXPECT_FLOAT_EQ(wma_input_get_level(mWma, 1), kStubLevelDb);
}

TEST_F(CApiInputTest, SourceGainAndFlagsRoundTripThroughTheirOwnNodeState) {
    attachInputNode(mWma);

    wma_input_set_source(mWma, 2);
    EXPECT_EQ(wma_input_get_source(mWma), 2);

    wma_input_set_gain(mWma, -6.0f);
    EXPECT_FLOAT_EQ(wma_input_get_gain(mWma), -6.0f);

    wma_input_set_noise_gate(mWma, true);
    EXPECT_TRUE(wma_input_is_noise_gate_enabled(mWma));

    wma_input_set_monitoring(mWma, true);
    EXPECT_TRUE(wma_input_is_monitoring_enabled(mWma));

    wma_input_set_monitoring_volume(mWma, 0.25f);
    EXPECT_FLOAT_EQ(wma_input_get_monitoring_volume(mWma), 0.25f);

    // Each setter must have touched only its own state.
    EXPECT_EQ(wma_input_get_source(mWma), 2);
    EXPECT_FLOAT_EQ(wma_input_get_gain(mWma), -6.0f);
}

TEST_F(CApiInputTest, AnOutOfRangeSourceIsRejectedInsteadOfStored) {
    attachInputNode(mWma);
    wma_input_set_source(mWma, 1);
    ASSERT_EQ(wma_input_get_source(mWma), 1);

    // Valid sources are 0 (DEFAULT), 1 (MIC), 2 (USB). The guard used to live in
    // the JNI, so it is the C API's job now — and the enum cast below it would
    // otherwise be undefined behaviour.
    wma_input_set_source(mWma, 3);
    EXPECT_EQ(wma_input_get_source(mWma), 1);

    wma_input_set_source(mWma, -1);
    EXPECT_EQ(wma_input_get_source(mWma), 1);
}

TEST_F(CApiInputTest, ReleaseDropsTheNodeAndTheQueriesGoBackToTheirDefaults) {
    attachInputNode(mWma);
    wma_input_set_gain(mWma, -6.0f);
    ASSERT_FLOAT_EQ(wma_input_get_level(mWma, 0), kStubLevelDb);

    wma_input_release(mWma);

    EXPECT_EQ(mWma->inputNode, nullptr);
    EXPECT_FLOAT_EQ(wma_input_get_level(mWma, 0), kNoNodeLevelDb);
    EXPECT_FLOAT_EQ(wma_input_get_gain(mWma), 0.0f);
}

// ===========================================================================
// 3. The batched metering snapshot
// ===========================================================================

TEST_F(CApiInputTest, TheMeteringSnapshotReportsNoDataInsteadOfZerosWithNoNode) {
    // nativeGetInputMeteringSnapshot returns null to Kotlin in exactly this
    // case, and the caller falls back to the individual getters. A buffer of
    // zeros would read as a real measurement of silence.
    float values[WMA_INPUT_METERING_VALUES];
    for (float& v : values) v = 12345.0f;

    EXPECT_FALSE(wma_input_get_metering_snapshot(mWma, values));

    for (float v : values) {
        EXPECT_FLOAT_EQ(v, 12345.0f) << "the buffer must be left untouched";
    }
}

TEST_F(CApiInputTest, TheMeteringSnapshotRejectsANullBuffer) {
    attachInputNode(mWma);
    EXPECT_FALSE(wma_input_get_metering_snapshot(mWma, nullptr));
}

TEST_F(CApiInputTest, TheMeteringSnapshotFillsEverySlotInTheDocumentedOrder) {
    attachInputNode(mWma);

    float values[WMA_INPUT_METERING_VALUES];
    for (float& v : values) v = 12345.0f;

    ASSERT_TRUE(wma_input_get_metering_snapshot(mWma, values));

    // dB levels first, then linear — the pair the Kotlin consumer is most
    // likely to transpose, and the stub gives them different values (-120 vs 0)
    // so a swap would show up here.
    EXPECT_FLOAT_EQ(values[0], kStubLevelDb);
    EXPECT_FLOAT_EQ(values[1], kStubLevelDb);
    EXPECT_FLOAT_EQ(values[2], 0.0f);
    EXPECT_FLOAT_EQ(values[3], 0.0f);
    EXPECT_FLOAT_EQ(values[4], 0.0f);  // clipping
    EXPECT_FLOAT_EQ(values[5], 0.0f);  // noise gate open
    EXPECT_FLOAT_EQ(values[6], 0.0f);  // latency ms

    for (float v : values) {
        EXPECT_NE(v, 12345.0f) << "every slot must be written, not just some";
    }
}

// ===========================================================================
// wma_input_start with nothing underneath
// ===========================================================================

TEST_F(CApiInputTest, StartReportsFailureRatherThanReopeningWithNoBackendSelected) {
    // The stub's startInputStream() fails, so this takes the BackendManager
    // fallback — which finds no active backend and gives up. The assertion that
    // matters is that it reports the failure instead of leaving the caller
    // believing the microphone is live.
    EXPECT_FALSE(wma_input_start(mWma));
    EXPECT_FALSE(wma_input_is_running(mWma));

    // It is still allowed to have created the node on the way — that is what
    // makes a later retry cheap.
    EXPECT_NE(mWma->inputNode, nullptr);
}

}  // namespace
}  // namespace wma_test
