/**
 * test_c_api_routing.cpp
 *
 * The tail of WA-2.5/2.6: routing (section 9), XY mapping / automation
 * (section 17) and backend selection (section 16). These 10 entry points were
 * never named by the category list — they lived in the script's "Otros" row and
 * only surfaced when the *complement* was enumerated instead of the list.
 *
 * The C API functions all existed already, so the migration was mechanical and
 * the diff in both directions came out clean. What was NOT clean is that this
 * surface had essentially zero host coverage: before this file, one call to
 * wma_select_backend() inside CApiFixture was the whole of it. So the tests
 * below are the first thing that pins these guards down.
 *
 * Two of the guards are load-bearing and two are decoration. Saying which is
 * which is the point — the looper taught us that a reader who cannot tell
 * treats the whole block as equally necessary:
 *
 *   LOAD-BEARING  the 0..5 range check in wma_set_routing_mode. RoutingMode has
 *                 exactly six enumerators and EffectChain::setRoutingMode stores
 *                 whatever it is handed, without validating. Drop this check and
 *                 a static_cast<RoutingMode>(99) reaches the atomic that the
 *                 render path switches on.
 *   LOAD-BEARING  the isfinite() check on the mapping bounds. Nothing downstream
 *                 looks at them: applyMappingCurve() computes
 *                 center + curved * range straight from mapMin/mapMax, so a NaN
 *                 bound becomes a NaN effect parameter.
 *   REDUNDANT     the 0..2 axis check. EffectChain::getMappingForAxis() is a
 *                 switch with `default: return nullptr`, and every caller bails
 *                 on null. The check is cheap and reads as intent; it is just
 *                 not what stops an out-of-range axis.
 *   REDUNDANT     the curve and polarity range checks. applyMappingCurve()'s
 *                 switch has a `default:` arm, and polarity is compared against
 *                 BIPOLAR with everything else falling through to unipolar.
 *
 * NOT COVERED — wma_set_depth_value(). See the note at the bottom of the file.
 *
 * TWO TESTS FAILED FIRST FOR THE WRONG REASON, and both were the harness:
 *
 *   The mapping tests originally used a range of [0.25, 0.75], on the assumption
 *   that an effect parameter is normalised. It is not: parameter 0 of a filter
 *   is cutoff in Hz and setCutoff() clamps to [20, 20000], so every mapped value
 *   arrived as 20 and all three assertions failed identically. A mapping range
 *   has to live inside the target parameter's own range or the clamp eats it —
 *   which is worth knowing before wiring an XY pad to anything.
 *
 *   The backend test asserted that selecting a missing backend fails. It does
 *   not. See AskingForUsbWithoutAUsbBackendSilentlyLandsOnTheSystemBackend.
 */

#include "support/CApiFixture.h"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kFilter = 0;

// RoutingMode, effects/EffectTypes.h — six enumerators, 0..5.
constexpr int kSerial   = 0;
constexpr int kParallel = 1;
constexpr int kFeedback = 5;

// Mapping axes and enums, section 17 of watermelon_audio.h.
constexpr int kAxisX     = 0;
constexpr int kAxisDepth = 2;
constexpr int kLinear    = 0;
constexpr int kUnipolar  = 0;

using CApiRoutingTest = CApiFixture;

// ===========================================================================
// Routing mode
// ===========================================================================

TEST_F(CApiRoutingTest, EveryRoutingModeRoundTrips) {
    // Default is SERIAL; setRoutingMode early-returns when the value is
    // unchanged, so start by proving the default rather than assuming it.
    EXPECT_EQ(wma_get_routing_mode(mWma), kSerial);

    for (int mode = 0; mode <= 5; ++mode) {
        wma_set_routing_mode(mWma, mode);
        EXPECT_EQ(wma_get_routing_mode(mWma), mode) << "routing mode " << mode;
    }
}

TEST_F(CApiRoutingTest, AnOutOfRangeRoutingModeIsRejectedRatherThanStored) {
    wma_set_routing_mode(mWma, kParallel);
    ASSERT_EQ(wma_get_routing_mode(mWma), kParallel);

    // EffectChain::setRoutingMode does not validate. This check is the only
    // thing between a bad int and the enum the render path switches on.
    wma_set_routing_mode(mWma, 6);
    EXPECT_EQ(wma_get_routing_mode(mWma), kParallel);

    wma_set_routing_mode(mWma, -1);
    EXPECT_EQ(wma_get_routing_mode(mWma), kParallel);

    wma_set_routing_mode(mWma, 99);
    EXPECT_EQ(wma_get_routing_mode(mWma), kParallel);
}

TEST_F(CApiRoutingTest, TheBoundaryModesAreInsideTheRangeNotOutside) {
    // An off-by-one in either direction of the guard would take one of these
    // away; the loop above would still pass if the guard were `> 6`.
    wma_set_routing_mode(mWma, kFeedback);
    EXPECT_EQ(wma_get_routing_mode(mWma), kFeedback);

    wma_set_routing_mode(mWma, kSerial);
    EXPECT_EQ(wma_get_routing_mode(mWma), kSerial);
}

// ===========================================================================
// XY mapping / automation
// ===========================================================================

TEST_F(CApiRoutingTest, AutomationDrivesTheMappedParameterAcrossTheConfiguredRange) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    wma_effect_set_param(mWma, 0, 0, 0.0f);

    wma_set_mapping_config(mWma, kAxisX, /*effect_index=*/0, /*param_id=*/0,
                           kLinear, kUnipolar,
                           /*map_min=*/200.0f, /*map_max=*/2000.0f,
                           /*inverted=*/false);

    wma_apply_automation(mWma, kAxisX, 0.0f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 200.0f);

    wma_apply_automation(mWma, kAxisX, 1.0f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);

    wma_apply_automation(mWma, kAxisX, 0.5f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 1100.0f);
}

TEST_F(CApiRoutingTest, InvertedMappingRunsTheRangeBackwards) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    wma_set_mapping_config(mWma, kAxisX, 0, 0, kLinear, kUnipolar,
                           200.0f, 2000.0f, /*inverted=*/true);

    wma_apply_automation(mWma, kAxisX, 0.0f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);

    wma_apply_automation(mWma, kAxisX, 1.0f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 200.0f);
}

TEST_F(CApiRoutingTest, ClearingAnAxisStopsItsAutomation) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    wma_set_mapping_config(mWma, kAxisX, 0, 0, kLinear, kUnipolar,
                           200.0f, 2000.0f, false);
    wma_apply_automation(mWma, kAxisX, 1.0f);
    ASSERT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);

    // clearMappingConfig stores effectIndex = -1, which applyAutomation reads
    // first and bails on. The parameter keeps its last automated value.
    wma_clear_mapping_config(mWma, kAxisX);
    wma_apply_automation(mWma, kAxisX, 0.0f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);
}

TEST_F(CApiRoutingTest, NonFiniteMappingBoundsAreRefusedInsteadOfPoisoningTheParameter) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    wma_set_mapping_config(mWma, kAxisX, 0, 0, kLinear, kUnipolar,
                           200.0f, 2000.0f, false);
    wma_apply_automation(mWma, kAxisX, 1.0f);
    ASSERT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Nothing downstream re-checks these. Without the guard the mapping config
    // would be overwritten and the next automation would write NaN into the
    // effect — and NaN propagates through the whole chain from there.
    wma_set_mapping_config(mWma, kAxisX, 0, 0, kLinear, kUnipolar, nan, 2000.0f, false);
    wma_apply_automation(mWma, kAxisX, 1.0f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);

    wma_set_mapping_config(mWma, kAxisX, 0, 0, kLinear, kUnipolar, 200.0f, inf, false);
    wma_apply_automation(mWma, kAxisX, 1.0f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);

    EXPECT_FALSE(std::isnan(wma_effect_get_param(mWma, 0, 0)));
}

TEST_F(CApiRoutingTest, AnOutOfRangeAxisTouchesNothing) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    wma_set_mapping_config(mWma, kAxisX, 0, 0, kLinear, kUnipolar,
                           200.0f, 2000.0f, false);
    wma_apply_automation(mWma, kAxisX, 1.0f);
    ASSERT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);

    // Redundant with getMappingForAxis()'s `default: return nullptr`, but the
    // observable contract is what this pins: axis 3 changes nothing.
    wma_set_mapping_config(mWma, /*axis=*/3, 0, 0, kLinear, kUnipolar,
                           0.0f, 1.0f, false);
    wma_clear_mapping_config(mWma, /*axis=*/-1);
    wma_apply_automation(mWma, /*axis=*/3, 0.0f);

    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 2000.0f);
}

TEST_F(CApiRoutingTest, TheDepthAxisIsAMappingAxisLikeTheOthers) {
    // Worth its own test only because wma_set_depth_value() shares the word
    // "depth" and does something entirely different — see the closing note.
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);

    wma_set_mapping_config(mWma, kAxisDepth, 0, 0, kLinear, kUnipolar,
                           500.0f, 5000.0f, false);

    wma_apply_automation(mWma, kAxisDepth, 1.0f);
    EXPECT_FLOAT_EQ(wma_effect_get_param(mWma, 0, 0), 5000.0f);
}

TEST_F(CApiRoutingTest, AutomationOnAnEffectThatIsGoneIsANoOp) {
    ASSERT_EQ(wma_effect_add(mWma, kFilter), 0);
    wma_set_mapping_config(mWma, kAxisX, /*effect_index=*/0, 0,
                           kLinear, kUnipolar, 200.0f, 2000.0f, false);

    wma_effect_remove(mWma, 0);

    // applyAutomation bounds-checks the mapped index against the live snapshot.
    // Nothing to assert but survival: the point is that it does not read past
    // an emptied chain.
    wma_apply_automation(mWma, kAxisX, 1.0f);
    EXPECT_EQ(wma_effect_chain_size(mWma), 0);
}

// ===========================================================================
// Null-handle sweep — the defaults that moved out of the JNI
// ===========================================================================

TEST_F(CApiRoutingTest, EveryEntryPointSurvivesANullEngine) {
    // Each of these used to be an `if (!g_jniState.engine) return;` in the JNI.
    // The default for the one query is 0 (SERIAL), which is what the JNI
    // returned by hand.
    EXPECT_EQ(wma_get_routing_mode(nullptr), 0);

    wma_set_routing_mode(nullptr, kParallel);
    wma_set_parallel_mix(nullptr, 0.5f);
    wma_set_feedback_amount(nullptr, 0.5f);
    wma_set_depth_value(nullptr, 0.5f);
    wma_set_mapping_config(nullptr, kAxisX, 0, 0, kLinear, kUnipolar, 0.0f, 1.0f, false);
    wma_clear_mapping_config(nullptr, kAxisX);
    wma_apply_automation(nullptr, kAxisX, 0.5f);

    SUCCEED() << "no crash on a null engine handle";
}

TEST_F(CApiRoutingTest, TheMixAndFeedbackSettersAcceptTheirWholeRange) {
    // EffectChain has no getter for either, so there is nothing to read back.
    // What this does pin is that neither setter validates or clamps — the JNI
    // did not either, so a value outside 0..1 reaches the chain unchanged, and
    // any future clamp would have to be a deliberate change rather than a
    // silent one.
    wma_set_parallel_mix(mWma, -1.0f);
    wma_set_parallel_mix(mWma, 2.0f);
    wma_set_feedback_amount(mWma, -1.0f);
    wma_set_feedback_amount(mWma, 2.0f);

    SUCCEED() << "documented as unvalidated, matching the pre-migration JNI";
}

// ===========================================================================
// Backend selection
// ===========================================================================

TEST_F(CApiRoutingTest, TheBackendTypeReflectsTheSelection) {
    // CApiFixture registers a fake as the system backend, which the manager
    // slots in where Oboe/CoreAudio would go — BackendType 1.
    ASSERT_TRUE(wma_select_backend(1));
    EXPECT_EQ(wma_get_backend_type(), 1);
}

TEST_F(CApiRoutingTest, AskingForUsbWithoutAUsbBackendSilentlyLandsOnTheSystemBackend) {
    // Written first as "selecting a missing backend fails", which is wrong —
    // and wrong in the direction that matters. BackendManager::selectBackend
    // does not refuse BackendType::LIBUSB when there is no USB backend: it
    // rewrites `type` to OBOE, points at the system backend, and returns TRUE.
    //
    // That is exactly what iOS gets under D4, where createUsbAudioBackend()
    // always returns null. So a caller that asks for USB on iOS is told it
    // succeeded, and only wma_get_backend_type() reveals it did not get USB.
    // The return value alone cannot be trusted to mean "you got what you asked
    // for"; the query is the source of truth.
    ASSERT_TRUE(wma_select_backend(1));

    EXPECT_TRUE(wma_select_backend(2)) << "the fallback reports success, not failure";
    EXPECT_EQ(wma_get_backend_type(), 1)
        << "and the current type is the system backend, not LIBUSB";
}

TEST_F(CApiRoutingTest, SelectingAnUnknownBackendIdIsRefused) {
    ASSERT_TRUE(wma_select_backend(1));

    // 3 is SPLIT, which is only selectable once createSplitBackend() has built
    // one — never on iOS. 99 is not a BackendType at all. Both hit a `return
    // false` and must leave the current backend alone.
    EXPECT_FALSE(wma_select_backend(3));
    EXPECT_EQ(wma_get_backend_type(), 1);

    EXPECT_FALSE(wma_select_backend(99));
    EXPECT_EQ(wma_get_backend_type(), 1);
}

TEST_F(CApiRoutingTest, TheBackendManagerToggleSurvivesBothStates) {
    // No getter; the observable effect is on the start path, covered by the
    // lifecycle suite. This only pins that the toggle itself is safe to flip.
    wma_set_use_backend_manager(mWma, false);
    wma_set_use_backend_manager(mWma, true);
    SUCCEED();
}

/*
 * ===========================================================================
 * NOT COVERED, and why — wma_set_depth_value()
 * ===========================================================================
 *
 * There is no test for it because there is nothing to observe. The value goes
 * Kotlin (coerceIn 0..1) -> JNI -> wma_set_depth_value (clamp 0..1) ->
 * AudioEngine::setDepthValue -> EffectChain::setDepthValue -> mDepthValue, and
 * mDepthValue is read by NOBODY: a grep over the whole engine finds the atomic's
 * declaration, its one store, and nothing else. It has no getter and never
 * reaches the render path.
 *
 * So the function is a dead store across all four layers, while its Kotlin doc
 * comment advertises "Set depth axis value. Lock-free real-time path." The real
 * depth axis is mapping axis 2, driven by wma_apply_automation — which is what
 * TheDepthAxisIsAMappingAxisLikeTheOthers above covers.
 *
 * It is migrated faithfully rather than fixed, deliberately: wiring mDepthValue
 * into the audio path would invent behaviour that no caller has ever heard, and
 * that is a product decision, not a migration. Writing an assertion here that
 * "passes" would be worse than saying this.
 */

}  // namespace
}  // namespace wma_test
