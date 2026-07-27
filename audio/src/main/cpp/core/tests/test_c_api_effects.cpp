/**
 * test_c_api_effects.cpp
 *
 * The effect-chain surface of the C API (section 8 of watermelon_audio.h), now
 * the only implementation: WA-2.6 turned the 14 matching JNI entry points into
 * one-line calls into these functions.
 *
 * Unlike the input suite, this one can assert real behaviour. EffectChain is
 * compiled into the host target for real — there is no stub — so adding an
 * effect actually adds one, and reordering actually reorders.
 *
 * The test that matters most is TheBatchSetterBumpsTheStateVersionExactlyOnce.
 * wma_effect_set_params_batch used to loop over the individual setter, and
 * AudioEngine::setParameter bumps the state version on every call — so setting
 * N parameters produced N bumps, and the Kotlin StateSynchronizer emits on each
 * one. A scene load was observed as a sequence of partial states instead of one
 * coherent state. That is AUD-6. The JNI has routed through setParametersBatch
 * (one bump, at the end) since it was fixed; the C API kept the broken shape,
 * so iOS still had the bug. Counting bumps is the only way to see it: every
 * parameter still ends up with the right value either way.
 */

#include "support/CApiFixture.h"

#include <gtest/gtest.h>

namespace wma_test {
namespace {

// EffectType values — see effects/EffectTypes.h.
constexpr int kFilter = 0;
constexpr int kReverb = 1;
constexpr int kDelay = 2;

using CApiEffectsTest = CApiFixture;

// ===========================================================================
// Chain manipulation
// ===========================================================================

TEST_F(CApiEffectsTest, AddReturnsTheNewIndexAndGrowsTheChain) {
    EXPECT_EQ(wma_effect_chain_size(mWma), 0);

    EXPECT_EQ(wma_effect_add(mWma, kFilter), 0);
    EXPECT_EQ(wma_effect_chain_size(mWma), 1);

    EXPECT_EQ(wma_effect_add(mWma, kReverb), 1);
    EXPECT_EQ(wma_effect_chain_size(mWma), 2);

    EXPECT_EQ(wma_effect_get_type(mWma, 0), kFilter);
    EXPECT_EQ(wma_effect_get_type(mWma, 1), kReverb);
}

TEST_F(CApiEffectsTest, RemoveShrinksTheChainAndShiftsWhatFollows) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    ASSERT_EQ(wma_effect_add(mWma, kReverb), 1);
    ASSERT_EQ(wma_effect_add(mWma, kDelay), 2);

    EXPECT_EQ(wma_effect_remove(mWma, 0), WMA_OK);

    EXPECT_EQ(wma_effect_chain_size(mWma), 2);
    EXPECT_EQ(wma_effect_get_type(mWma, 0), kReverb);
    EXPECT_EQ(wma_effect_get_type(mWma, 1), kDelay);
}

TEST_F(CApiEffectsTest, ClearAllEmptiesTheChain) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    ASSERT_EQ(wma_effect_add(mWma, kReverb), 1);

    EXPECT_EQ(wma_effect_clear_all(mWma), WMA_OK);
    EXPECT_EQ(wma_effect_chain_size(mWma), 0);
    EXPECT_EQ(wma_effect_get_type(mWma, 0), -1);
}

TEST_F(CApiEffectsTest, ReorderMovesTheEffectRatherThanSwappingTypes) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    ASSERT_EQ(wma_effect_add(mWma, kReverb), 1);
    ASSERT_EQ(wma_effect_add(mWma, kDelay), 2);

    EXPECT_EQ(wma_effect_reorder(mWma, 0, 2), WMA_OK);

    EXPECT_EQ(wma_effect_chain_size(mWma), 3);
    EXPECT_EQ(wma_effect_get_type(mWma, 0), kReverb);
    EXPECT_EQ(wma_effect_get_type(mWma, 1), kDelay);
    EXPECT_EQ(wma_effect_get_type(mWma, 2), kFilter);
}

// ===========================================================================
// Error codes — the JNI returned these by hand and now forwards them
// ===========================================================================

TEST_F(CApiEffectsTest, AnUnknownEffectTypeIsRejected) {
    EXPECT_EQ(wma_effect_add(mWma, -1), WMA_ERROR_INVALID_EFFECT_TYPE);
    EXPECT_EQ(wma_effect_add(mWma, 9999), WMA_ERROR_INVALID_EFFECT_TYPE);
    EXPECT_EQ(wma_effect_chain_size(mWma), 0);
}

TEST_F(CApiEffectsTest, AnOutOfRangeIndexIsRejectedByEveryIndexedCall) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    EXPECT_EQ(wma_effect_remove(mWma, 1), WMA_ERROR_INVALID_EFFECT_INDEX);
    EXPECT_EQ(wma_effect_remove(mWma, -1), WMA_ERROR_INVALID_EFFECT_INDEX);
    EXPECT_EQ(wma_effect_set_param(mWma, 1, 0, 0.5f), WMA_ERROR_INVALID_EFFECT_INDEX);
    EXPECT_EQ(wma_effect_set_bypass(mWma, 1, true), WMA_ERROR_INVALID_EFFECT_INDEX);
    EXPECT_EQ(wma_effect_reorder(mWma, 0, 1), WMA_ERROR_INVALID_EFFECT_INDEX);
    EXPECT_EQ(wma_effect_reorder(mWma, 1, 0), WMA_ERROR_INVALID_EFFECT_INDEX);

    // The chain must be untouched by all of that.
    EXPECT_EQ(wma_effect_chain_size(mWma), 1);
    EXPECT_EQ(wma_effect_get_type(mWma, 0), kFilter);
}

TEST_F(CApiEffectsTest, ANonFiniteParameterValueIsRejectedRatherThanStored) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // A NaN reaching a filter coefficient poisons the whole chain: every sample
    // downstream comes out NaN and the output goes silent until it is rebuilt.
    EXPECT_EQ(wma_effect_set_param(mWma, 0, 0, inf), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_effect_set_param(mWma, 0, 0, nan), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_effect_set_param(mWma, 0, -1, 0.5f), WMA_ERROR_INVALID_PARAMETER_ID);
}

// ===========================================================================
// Bypass
// ===========================================================================

TEST_F(CApiEffectsTest, PerEffectBypassAndGlobalBypassAreSeparateState) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    ASSERT_EQ(wma_effect_add(mWma, kReverb), 1);

    EXPECT_EQ(wma_effect_set_bypass(mWma, 0, true), WMA_OK);
    EXPECT_TRUE(wma_effect_is_bypassed(mWma, 0));
    EXPECT_FALSE(wma_effect_is_bypassed(mWma, 1));
    EXPECT_FALSE(wma_effect_is_global_bypassed(mWma))
        << "bypassing one effect must not read as bypassing the chain";

    EXPECT_EQ(wma_effect_set_global_bypass(mWma, true), WMA_OK);
    EXPECT_TRUE(wma_effect_is_global_bypassed(mWma));
    EXPECT_FALSE(wma_effect_is_bypassed(mWma, 1))
        << "and the global flag must not rewrite the per-effect ones";
}

// ===========================================================================
// AUD-6 — the batch setters must bump the state version exactly once
// ===========================================================================

TEST_F(CApiEffectsTest, TheIndividualSetterBumpsOncePerCall) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    // Establishes the baseline the batch tests are measured against: without
    // this, "one bump" could just mean the version never moves at all.
    const uint64_t before = wma_get_state_version(mWma);
    wma_effect_set_param(mWma, 0, 0, 0.1f);
    wma_effect_set_param(mWma, 0, 1, 0.2f);
    wma_effect_set_param(mWma, 0, 2, 0.3f);

    EXPECT_EQ(wma_get_state_version(mWma) - before, 3u);
}

TEST_F(CApiEffectsTest, TheBatchSetterBumpsTheStateVersionExactlyOnce) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    const int ids[] = {0, 1, 2, 3};
    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};

    const uint64_t before = wma_get_state_version(mWma);
    EXPECT_EQ(wma_effect_set_params_batch(mWma, 0, ids, values, 4), WMA_OK);

    // Was 4 before the fix — one per parameter — which is what let the Kotlin
    // synchronizer observe a half-loaded scene.
    EXPECT_EQ(wma_get_state_version(mWma) - before, 1u);
}

TEST_F(CApiEffectsTest, TheMultiEffectBatchAlsoBumpsExactlyOnce) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    ASSERT_EQ(wma_effect_add(mWma, kReverb), 1);
    ASSERT_EQ(wma_effect_add(mWma, kDelay), 2);

    const int effects[] = {0, 1, 2, 0, 1};
    const int ids[] = {0, 0, 0, 1, 1};
    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};

    const uint64_t before = wma_get_state_version(mWma);
    EXPECT_EQ(wma_effect_set_params_multi(mWma, effects, ids, values, 5), WMA_OK);

    EXPECT_EQ(wma_get_state_version(mWma) - before, 1u);
}

TEST_F(CApiEffectsTest, TheBatchSetterAppliesExactlyWhatTheIndividualSetterWould) {
    // The bump count is the point, but it would be a hollow win if the batch
    // path dropped or mangled updates on the way.
    //
    // The assertion is against the individual setter rather than against the
    // literal input, because effects clamp: parameter 0 of a filter is a cutoff
    // in Hz, so 0.25 comes back as 20. Comparing the two paths is both the
    // invariant that actually matters and immune to that.
    const int ids[] = {0, 1};
    const float values[] = {0.25f, 0.75f};

    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    wma_effect_set_param(mWma, 0, ids[0], values[0]);
    wma_effect_set_param(mWma, 0, ids[1], values[1]);
    const float expected0 = wma_effect_get_param(mWma, 0, ids[0]);
    const float expected1 = wma_effect_get_param(mWma, 0, ids[1]);

    ASSERT_EQ(wma_effect_clear_all(mWma), WMA_OK);
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    ASSERT_NE(wma_effect_get_param(mWma, 0, ids[0]), expected0)
        << "a fresh effect must not already hold the value, or this proves nothing";

    ASSERT_EQ(wma_effect_set_params_batch(mWma, 0, ids, values, 2), WMA_OK);

    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, ids[0]), expected0);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, ids[1]), expected1);
}

TEST_F(CApiEffectsTest, AnEmptyBatchIsNotAnErrorAndDoesNotBump) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    const uint64_t before = wma_get_state_version(mWma);

    EXPECT_EQ(wma_effect_set_params_batch(mWma, 0, nullptr, nullptr, 0), WMA_OK);
    EXPECT_EQ(wma_effect_set_params_multi(mWma, nullptr, nullptr, nullptr, 0), WMA_OK);

    EXPECT_EQ(wma_get_state_version(mWma), before);
}

TEST_F(CApiEffectsTest, TheMultiEffectBatchSkipsBadEntriesInsteadOfDroppingTheBatch) {
    // Same clamping caveat as above: compare against what the individual setter
    // produces, not against the literal 0.25.
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    wma_effect_set_param(mWma, 0, 0, 0.25f);
    const float expected = wma_effect_get_param(mWma, 0, 0);

    ASSERT_EQ(wma_effect_clear_all(mWma), WMA_OK);
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    ASSERT_NE(wma_effect_get_param(mWma, 0, 0), expected);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Effect 7 is not in the chain and the third value is not finite. A scene
    // pointing at an effect the user has since removed must not cost the other
    // updates.
    const int effects[] = {0, 7, 0};
    const int ids[] = {0, 0, 1};
    const float values[] = {0.25f, 0.5f, nan};

    EXPECT_EQ(wma_effect_set_params_multi(mWma, effects, ids, values, 3), WMA_OK);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), expected);
}

// ===========================================================================
// Null handle
// ===========================================================================

TEST(CApiEffectsNullHandle, EveryCallReturnsTheValueTheJniUsedToReturnByHand) {
    EXPECT_EQ(wma_effect_add(nullptr, kFilter), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_effect_remove(nullptr, 0), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_effect_clear_all(nullptr), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_effect_set_param(nullptr, 0, 0, 0.5f), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_effect_set_bypass(nullptr, 0, true), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_effect_set_global_bypass(nullptr, true), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_effect_reorder(nullptr, 0, 1), WMA_ERROR_NOT_INITIALIZED);

    const int ids[] = {0};
    const float values[] = {0.5f};
    EXPECT_EQ(wma_effect_set_params_batch(nullptr, 0, ids, values, 1),
              WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_effect_set_params_multi(nullptr, ids, ids, values, 1),
              WMA_ERROR_NOT_INITIALIZED);

    // Queries answer with their documented defaults, not with an error code.
    EXPECT_EQ(wma_effect_chain_size(nullptr), 0);
    EXPECT_EQ(wma_effect_get_type(nullptr, 0), -1);
    EXPECT_FLOAT_EQ(wma_effect_get_param(nullptr, 0, 0), 0.0f);
    EXPECT_FALSE(wma_effect_is_bypassed(nullptr, 0));
    EXPECT_FALSE(wma_effect_is_global_bypassed(nullptr));
}

}  // namespace
}  // namespace wma_test
