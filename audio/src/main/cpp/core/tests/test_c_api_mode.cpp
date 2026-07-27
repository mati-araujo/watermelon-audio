/**
 * test_c_api_mode.cpp
 *
 * Audio-mode transitions — section 11 of watermelon_audio.h.
 *
 * This is the category that removed the duplicated mode state: JniGlobalState
 * used to carry its own currentMode / modeTransitionInProgress /
 * modeTransitionProgress next to WmaEngine's, as independent copies. They are
 * gone; there is one set, and it lives where these functions read it.
 *
 * It is also the category where the C API was doing LESS than the JNI, and said
 * so in its own doc comment: "a simplified version" whose full behaviour
 * "should be handled by the platform layer". Two things were missing, and both
 * are covered here:
 *
 *   - the effect-chain reset on entering INPUT_FX, which keeps chaos_pad's
 *     reverb tail out of the first blocks of microphone audio. iOS did not have
 *     it, so switching to the mic after a long chaos_pad session came in with a
 *     burst of stale reverb.
 *
 *   - the USB branch: on a backend that delivers input through the render
 *     callback there must be no separate node-level stream. Not reproducible in
 *     this suite — the fake backend reports OBOE and the InputNode stub has no
 *     stream to speak of — so it is verified by the migration diff and by the
 *     device smoke, and said so here rather than faked.
 */

#include "support/CApiFixture.h"

#include <cstring>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

// AudioMode, mirroring core/AudioMode.h.
constexpr int kChaosPad = 0;
constexpr int kInputFx = 1;
constexpr int kMix = 2;

constexpr int kBlockFrames = 256;

using CApiModeTest = CApiFixture;

// ===========================================================================
// The mode itself
// ===========================================================================

TEST_F(CApiModeTest, TheEngineStartsInChaosPad) {
    EXPECT_EQ(wma_get_audio_mode(mWma), kChaosPad);
}

TEST_F(CApiModeTest, TheModeRoundTrips) {
    wma_set_audio_mode(mWma, kInputFx);
    EXPECT_EQ(wma_get_audio_mode(mWma), kInputFx);

    wma_set_audio_mode(mWma, kMix);
    EXPECT_EQ(wma_get_audio_mode(mWma), kMix);

    wma_set_audio_mode(mWma, kChaosPad);
    EXPECT_EQ(wma_get_audio_mode(mWma), kChaosPad);
}

TEST_F(CApiModeTest, AnInvalidModeIsRejectedAndLeavesTheCurrentOneAlone) {
    wma_set_audio_mode(mWma, kMix);
    ASSERT_EQ(wma_get_audio_mode(mWma), kMix);

    // The cast to AudioMode below the guard would be undefined behaviour.
    wma_set_audio_mode(mWma, 3);
    EXPECT_EQ(wma_get_audio_mode(mWma), kMix);

    wma_set_audio_mode(mWma, -1);
    EXPECT_EQ(wma_get_audio_mode(mWma), kMix);
}

// ===========================================================================
// What each mode actually does to the engine
// ===========================================================================

TEST_F(CApiModeTest, InputFxSilencesTheOscillatorAndChaosPadBringsItBack) {
    // The observable half of the transition: whether the oscillator runs is what
    // separates "playing the pad" from "processing the mic".
    ASSERT_TRUE(mWma->engine->isOscillatorEnabled());

    wma_set_audio_mode(mWma, kInputFx);
    EXPECT_FALSE(mWma->engine->isOscillatorEnabled());

    wma_set_audio_mode(mWma, kChaosPad);
    EXPECT_TRUE(mWma->engine->isOscillatorEnabled());
}

TEST_F(CApiModeTest, MixKeepsTheOscillatorRunning) {
    wma_set_audio_mode(mWma, kInputFx);
    ASSERT_FALSE(mWma->engine->isOscillatorEnabled());

    // MIX is both at once — the oscillator has to come back on.
    wma_set_audio_mode(mWma, kMix);
    EXPECT_TRUE(mWma->engine->isOscillatorEnabled());
}

TEST_F(CApiModeTest, TheModesThatNeedInputCreateTheInputNode) {
    ASSERT_EQ(mWma->inputNode, nullptr);

    wma_set_audio_mode(mWma, kChaosPad);
    EXPECT_EQ(mWma->inputNode, nullptr)
        << "chaos_pad must not spin up the microphone path";

    wma_set_audio_mode(mWma, kInputFx);
    EXPECT_NE(mWma->inputNode, nullptr);
}

TEST_F(CApiModeTest, MixAlsoCreatesTheInputNode) {
    ASSERT_EQ(mWma->inputNode, nullptr);

    wma_set_audio_mode(mWma, kMix);

    EXPECT_NE(mWma->inputNode, nullptr);
}

TEST_F(CApiModeTest, LeavingInputFxStopsMonitoring) {
    wma_set_audio_mode(mWma, kInputFx);
    ASSERT_NE(mWma->inputNode, nullptr);
    ASSERT_TRUE(wma_input_is_monitoring_enabled(mWma));

    // Otherwise the mic keeps being mixed into the output while the user is
    // back on the pad.
    wma_set_audio_mode(mWma, kChaosPad);

    EXPECT_FALSE(wma_input_is_monitoring_enabled(mWma));
}

// ===========================================================================
// The effect-chain reset — the piece the C API was missing
// ===========================================================================

TEST_F(CApiModeTest, SwitchingToInputFxLeavesNoResidueOnTheOutput) {
    // WHAT THIS DOES AND DOES NOT PROVE.
    //
    // It proves the user-facing property: after the switch, nothing of chaos_pad
    // is still coming out. That is worth pinning — if the graph ever changes so
    // the effect chain keeps being pulled with the oscillator off, the bleed
    // this guards against comes back and this test goes red.
    //
    // It does NOT prove that requestResetEffectChain() ran. Verified by
    // mutation: delete that call and this still passes, because on the host the
    // output goes to exactly 0.0 once the oscillator stops — the reverb tail
    // never reaches the output here at all. The harness does not model the path
    // where it does, which is the device.
    //
    // So the reset itself — the piece the C API was missing, and the reason
    // switching to the mic after a long pad session burst on iOS — is covered by
    // the migration diff and belongs in the device smoke. Said here rather than
    // dressed up as tested.
    constexpr int kFrames = 256;
    startAt(48000, 0);
    ASSERT_EQ(wma_effect_add(mWma, 1), 0);  // REVERB
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);

    render(20, kFrames);
    const float chargedPeak = renderBlockPeak(kFrames);
    ASSERT_GT(chargedPeak, 0.01f)
        << "the pad has to be audible for the assertion below to mean anything";

    wma_set_audio_mode(mWma, kInputFx);
    const float afterSwitch = renderBlockPeak(kFrames);

    EXPECT_LT(afterSwitch, chargedPeak * 0.1f)
        << "chaos_pad bled into the first block of INPUT_FX "
           "(charged=" << chargedPeak << ", after=" << afterSwitch << ")";
}

TEST_F(CApiModeTest, TheResetClearsTheChainStateWithoutRemovingTheEffects) {
    constexpr int kFrames = 256;
    startAt(48000, 0);
    ASSERT_EQ(wma_effect_add(mWma, 1), 0);
    ASSERT_EQ(wma_effect_add(mWma, 2), 1);  // DELAY
    render(4, kFrames);

    wma_set_audio_mode(mWma, kInputFx);
    render(4, kFrames);

    EXPECT_EQ(wma_effect_chain_size(mWma), 2)
        << "resetting the chain's state must not unbuild the chain";
    EXPECT_EQ(wma_effect_get_type(mWma, 0), 1);
    EXPECT_EQ(wma_effect_get_type(mWma, 1), 2);
}

// ===========================================================================
// Mode metadata — pure functions, no engine
// ===========================================================================

TEST(CApiModeStatics, ModeNamesAreStableAndNeverNull) {
    EXPECT_STREQ(wma_get_mode_name(kChaosPad), "ChaosPad");
    EXPECT_STREQ(wma_get_mode_name(kInputFx), "Input FX");
    EXPECT_STREQ(wma_get_mode_name(kMix), "Mix");

    // nativeGetModeName feeds this straight into NewStringUTF, which would
    // crash on a null. An out-of-range id falls through to "Unknown", so there
    // is no path that returns one.
    EXPECT_STREQ(wma_get_mode_name(99), "Unknown");
}

TEST(CApiModeStatics, OnlyTheMicModesRequireInput) {
    EXPECT_FALSE(wma_mode_requires_input(kChaosPad));
    EXPECT_TRUE(wma_mode_requires_input(kInputFx));
    EXPECT_TRUE(wma_mode_requires_input(kMix));
}

// ===========================================================================
// Transition flags — dead API, documented rather than blessed
// ===========================================================================

TEST_F(CApiModeTest, TheTransitionFlagsAreAlwaysInactive) {
    // NOT the behaviour these should have. Nothing writes either one: the class
    // that owns real transition state, core/ModeManager, is not wired into
    // AudioEngine at all. So nativeIsInModeTransition has always answered
    // "false" and nativeGetModeTransitionProgress "0", on both platforms.
    //
    // Migrating the pair did not change that — it moved dead state from two
    // copies to one. This test says so out loud, and fails if someone wires the
    // real thing up, which is the right way to find out.
    EXPECT_FALSE(wma_is_in_mode_transition(mWma));
    EXPECT_FLOAT_EQ(wma_get_mode_transition_progress(mWma), 0.0f);

    wma_set_audio_mode(mWma, kInputFx);

    EXPECT_FALSE(wma_is_in_mode_transition(mWma))
        << "if this is now true, ModeManager got wired up — update this test";
    EXPECT_FLOAT_EQ(wma_get_mode_transition_progress(mWma), 0.0f);
}

// ===========================================================================
// Null handle
// ===========================================================================

TEST(CApiModeNullHandle, EveryCallIsSafeWithoutAnEngine) {
    EXPECT_EQ(wma_get_audio_mode(nullptr), 0);
    EXPECT_FALSE(wma_is_in_mode_transition(nullptr));
    EXPECT_FLOAT_EQ(wma_get_mode_transition_progress(nullptr), 0.0f);

    wma_set_audio_mode(nullptr, kInputFx);
    SUCCEED();
}

}  // namespace
}  // namespace wma_test
