/**
 * test_c_api_synth.cpp
 *
 * The sound-producing side of the C API, everything upstream of the effect
 * chain: sections 4 (XY / Oscillator), 5 (Engine synth), 6 (SoundFont),
 * 15 (Vocoder) and 18 (Arpeggiator). WA-2.6 turned the 39 matching JNI entry
 * points into one-line calls into these.
 *
 * WHAT IS AND IS NOT COVERED, and why.
 *
 * Covered: the synth engine round-trip, the arpeggiator's enabled flag, the
 * SoundFont argument guards, and the null-handle contract for all of it — the
 * last one being what the JNI used to spell out by hand and now delegates.
 *
 * NOT covered, for lack of a way to observe it rather than for lack of trying:
 *
 *   - The oscillator-type range check (0–4) and the frequency-range validation.
 *     Both moved out of the JNI into wma_set_oscillator_type /
 *     wma_set_frequency_range, and neither has a getter in the C API, so a
 *     rejected value is indistinguishable from an accepted one from out here.
 *     Adding getters just to test them would be inventing API; leaving a test
 *     that cannot fail would be worse. Flagged instead.
 *
 *   - Anything that needs a real .sf2. The negative paths below need no
 *     fixture, but the loaders' success paths do, and that fixture is the
 *     already-registered debt on the three loadSoundFont* functions.
 *
 *   - The arpeggiator's step counters. getCurrentStep / getTotalSteps are UI
 *     mirrors written by the audio thread, so they only move once blocks are
 *     rendered through a running sequencer. Out of scope for a C API contract
 *     test; the sequencer's own behaviour is not what changed here.
 */

#include "support/CApiFixture.h"

#include <gtest/gtest.h>

namespace wma_test {
namespace {

// Engine type IDs — see the doc on wma_set_engine_type.
constexpr int kClassic = 0;
constexpr int kKarplusStrong = 1;
constexpr int kFm = 2;

using CApiSynthTest = CApiFixture;

// ===========================================================================
// Section 5 — synth engine
// ===========================================================================

TEST_F(CApiSynthTest, TheEngineTypeRoundTrips) {
    // Also the proof that these calls reach the engine at all: everything else
    // in sections 4 and 5 is write-only from out here.
    EXPECT_EQ(wma_get_engine_type(mWma), kClassic);

    wma_set_engine_type(mWma, kKarplusStrong);
    EXPECT_EQ(wma_get_engine_type(mWma), kKarplusStrong);

    wma_set_engine_type(mWma, kFm);
    EXPECT_EQ(wma_get_engine_type(mWma), kFm);

    wma_set_engine_type(mWma, kClassic);
    EXPECT_EQ(wma_get_engine_type(mWma), kClassic);
}

TEST_F(CApiSynthTest, TheWriteOnlySettersAreSurvivable) {
    // No getters, so this is a smoke test and says so. It still earns its place:
    // it is what catches a wma_* wired to the wrong AudioEngine method, which
    // would fail to compile, or one that trips over an edge value.
    wma_set_xy(mWma, 0.5f, 0.5f);
    wma_set_xy(mWma, -1.0f, 2.0f);          // out of range, clamped
    wma_set_frequency_amplitude(mWma, 440.0f, 0.8f);
    wma_set_frequency_amplitude(mWma, 0.0f, 5.0f);  // out of range, clamped
    wma_set_frequency_range(mWma, 100.0f, 1000.0f);
    wma_set_frequency_range(mWma, 1000.0f, 100.0f); // inverted, rejected
    wma_set_oscillator_type(mWma, 2);
    wma_set_oscillator_type(mWma, 99);      // out of range, rejected
    wma_set_engine_param(mWma, 0, 0.5f);

    // The engine must still be usable afterwards.
    EXPECT_TRUE(wma_is_initialized(mWma));
    EXPECT_EQ(wma_get_engine_type(mWma), kClassic);
}

// ===========================================================================
// Section 6 — SoundFont, the paths that need no .sf2
// ===========================================================================

TEST_F(CApiSynthTest, NoSoundFontIsLoadedOnAFreshEngine) {
    EXPECT_FALSE(wma_sf_is_loaded(mWma));
    EXPECT_EQ(wma_sf_get_preset_count(mWma), 0);
    EXPECT_EQ(wma_sf_get_preset_name(mWma, 0), nullptr);
}

TEST_F(CApiSynthTest, BadLoaderArgumentsFailAndLoadNothing) {
    // A CONTRACT test, not a test of where the contract is enforced.
    //
    // wma_sf_load_fd guards fd/offset/length up front, which the JNI did not.
    // Deleting that guard does NOT fail this test: the dispatcher underneath
    // rejects the same values on its own, so from out here the two are
    // indistinguishable. Verified by mutation — worth stating so nobody reads
    // the fd lines below as proof the guard is doing something.
    //
    // The null checks are different: without them a null path or buffer reaches
    // tsf, and that is a crash rather than a false. Those the test does own.
    EXPECT_FALSE(wma_sf_load_path(mWma, nullptr));

    EXPECT_FALSE(wma_sf_load_fd(mWma, -1, 0, 1024));   // no fd
    EXPECT_FALSE(wma_sf_load_fd(mWma, 0, -1, 1024));   // negative offset
    EXPECT_FALSE(wma_sf_load_fd(mWma, 0, 0, 0));       // empty region
    EXPECT_FALSE(wma_sf_load_fd(mWma, 0, 0, -1));      // negative length

    const char bytes[4] = {0, 0, 0, 0};
    EXPECT_FALSE(wma_sf_load_data(mWma, nullptr, 4));  // no data
    EXPECT_FALSE(wma_sf_load_data(mWma, bytes, 0));    // empty
    EXPECT_FALSE(wma_sf_load_data(mWma, bytes, -1));   // negative size

    EXPECT_FALSE(wma_sf_is_loaded(mWma)) << "and none of that may have loaded anything";
}

TEST_F(CApiSynthTest, ThePresetQueriesLeaveTheirOutParamsAloneWhenThereIsNoPreset) {
    // The JNI returns null to Kotlin in this case rather than an array, so the
    // caller can tell "no answer" from "bank 0, program 0" — which is a real
    // preset, usually the piano.
    int minKey = -7, maxKey = -7;
    EXPECT_FALSE(wma_sf_get_preset_key_range(mWma, 0, &minKey, &maxKey));
    EXPECT_EQ(minKey, -7);
    EXPECT_EQ(maxKey, -7);

    int bank = -7, program = -7;
    EXPECT_FALSE(wma_sf_get_preset_bank_program(mWma, 0, &bank, &program));
    EXPECT_EQ(bank, -7);
    EXPECT_EQ(program, -7);
}

TEST_F(CApiSynthTest, ThePresetQueriesAcceptNullOutParams) {
    // The out params are individually optional — a caller wanting only the bank
    // must not have to supply somewhere to put the program.
    EXPECT_FALSE(wma_sf_get_preset_key_range(mWma, 0, nullptr, nullptr));
    EXPECT_FALSE(wma_sf_get_preset_bank_program(mWma, 0, nullptr, nullptr));
    SUCCEED();
}

// ===========================================================================
// Section 18 — arpeggiator
// ===========================================================================

TEST_F(CApiSynthTest, TheArpeggiatorEnabledFlagRoundTrips) {
    EXPECT_FALSE(wma_arp_is_enabled(mWma));

    wma_arp_set_enabled(mWma, true);
    EXPECT_TRUE(wma_arp_is_enabled(mWma));

    wma_arp_set_enabled(mWma, false);
    EXPECT_FALSE(wma_arp_is_enabled(mWma));
}

TEST_F(CApiSynthTest, TheArpeggiatorSettersDoNotDisturbTheEnabledFlag) {
    // Each one is a distinct ArpSequencer method and they are easy to
    // transpose; if any of them landed on setEnabled the flag would move.
    wma_arp_set_enabled(mWma, true);
    ASSERT_TRUE(wma_arp_is_enabled(mWma));

    wma_arp_set_pattern(mWma, 2);
    wma_arp_set_subdivision(mWma, 0.25f);
    wma_arp_set_octave_range(mWma, 3);
    wma_arp_set_gate_length(mWma, 0.5f);
    wma_arp_set_swing(mWma, 0.2f);
    wma_arp_set_latch(mWma, true);
    wma_arp_set_velocity(mWma, 0.9f);
    wma_arp_set_velocity_variation(mWma, 0.1f);
    wma_arp_set_probability(mWma, 0.8f);
    wma_arp_set_touch_active(mWma, true);
    wma_arp_set_base_freq(mWma, 220.0f);
    wma_arp_set_ratchet(mWma, true);
    wma_arp_regenerate(mWma);

    EXPECT_TRUE(wma_arp_is_enabled(mWma));
}

TEST_F(CApiSynthTest, TheScaleIntervalsSetterHandlesAnEmptyOrNullArray) {
    const int major[] = {0, 2, 4, 5, 7, 9, 11};
    wma_arp_set_scale_intervals(mWma, major, 7);

    // More than the 12 the sequencer keeps, and the degenerate inputs.
    const int tooMany[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    wma_arp_set_scale_intervals(mWma, tooMany, 15);
    wma_arp_set_scale_intervals(mWma, nullptr, 7);
    wma_arp_set_scale_intervals(mWma, major, 0);
    wma_arp_set_scale_intervals(mWma, major, -1);

    EXPECT_TRUE(wma_is_initialized(mWma));
}

TEST_F(CApiSynthTest, TheArpeggiatorStartsClosedAndAtStepZero) {
    EXPECT_FALSE(wma_arp_is_gate_open(mWma));
    EXPECT_EQ(wma_arp_get_current_step(mWma), 0);
    EXPECT_GE(wma_arp_get_total_steps(mWma), 0);
}

// ===========================================================================
// Section 15 — vocoder
// ===========================================================================

TEST_F(CApiSynthTest, TheVocoderReportsNoEffectUntilOneIsInTheChain) {
    EXPECT_FALSE(wma_vocoder_has_effect(mWma));

    wma_vocoder_set_carrier_source(mWma, true);
    wma_vocoder_set_carrier_freq(mWma, 440.0f);
    wma_vocoder_set_carrier_freq(mWma, 999999.0f);  // clamped to 2000
    wma_vocoder_set_modulator_source(mWma, true);

    EXPECT_FALSE(wma_vocoder_has_effect(mWma))
        << "configuring the vocoder must not conjure the effect into the chain";
}

// ===========================================================================
// Null handle
// ===========================================================================

TEST(CApiSynthNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    EXPECT_EQ(wma_get_engine_type(nullptr), 0);

    EXPECT_FALSE(wma_sf_is_loaded(nullptr));
    EXPECT_EQ(wma_sf_get_preset_count(nullptr), 0);
    EXPECT_EQ(wma_sf_get_preset_name(nullptr, 0), nullptr);
    EXPECT_FALSE(wma_sf_load_path(nullptr, "/tmp/none.sf2"));
    EXPECT_FALSE(wma_sf_load_fd(nullptr, 0, 0, 1024));
    EXPECT_FALSE(wma_sf_load_data(nullptr, "x", 1));

    int a = -7, b = -7;
    EXPECT_FALSE(wma_sf_get_preset_key_range(nullptr, 0, &a, &b));
    EXPECT_FALSE(wma_sf_get_preset_bank_program(nullptr, 0, &a, &b));
    EXPECT_EQ(a, -7);
    EXPECT_EQ(b, -7);

    EXPECT_FALSE(wma_arp_is_enabled(nullptr));
    EXPECT_EQ(wma_arp_get_current_step(nullptr), 0);
    EXPECT_EQ(wma_arp_get_total_steps(nullptr), 0);
    EXPECT_FALSE(wma_arp_is_gate_open(nullptr));

    EXPECT_FALSE(wma_vocoder_has_effect(nullptr));
}

TEST(CApiSynthNullHandle, EveryMutatorIsANoOpRatherThanACrash) {
    wma_set_xy(nullptr, 0.5f, 0.5f);
    wma_set_frequency_amplitude(nullptr, 440.0f, 0.5f);
    wma_set_frequency_range(nullptr, 100.0f, 1000.0f);
    wma_set_oscillator_type(nullptr, 1);
    wma_set_engine_type(nullptr, 1);
    wma_set_engine_param(nullptr, 0, 0.5f);

    wma_sf_unload(nullptr);
    wma_sf_set_preset(nullptr, 0);

    const int major[] = {0, 2, 4, 5, 7, 9, 11};
    wma_arp_set_enabled(nullptr, true);
    wma_arp_set_pattern(nullptr, 1);
    wma_arp_set_subdivision(nullptr, 0.25f);
    wma_arp_set_octave_range(nullptr, 2);
    wma_arp_set_gate_length(nullptr, 0.5f);
    wma_arp_set_swing(nullptr, 0.2f);
    wma_arp_set_latch(nullptr, true);
    wma_arp_set_velocity(nullptr, 0.9f);
    wma_arp_set_velocity_variation(nullptr, 0.1f);
    wma_arp_set_probability(nullptr, 0.8f);
    wma_arp_set_scale_intervals(nullptr, major, 7);
    wma_arp_set_touch_active(nullptr, true);
    wma_arp_set_base_freq(nullptr, 220.0f);
    wma_arp_set_ratchet(nullptr, true);
    wma_arp_regenerate(nullptr);

    wma_vocoder_set_carrier_source(nullptr, true);
    wma_vocoder_set_carrier_freq(nullptr, 440.0f);
    wma_vocoder_set_modulator_source(nullptr, true);
    SUCCEED();
}

}  // namespace
}  // namespace wma_test
