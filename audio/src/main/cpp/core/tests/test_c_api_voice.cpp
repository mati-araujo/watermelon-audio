/**
 * test_c_api_voice.cpp
 *
 * The polyphony side of the C API: sections 7 (Voice Filter), 13 (Dual Touch)
 * and 14 (Voice System), plus the four SoundFont note functions from section 6.
 * WA-2.6 turned the 21 matching JNI entry points into calls into these.
 *
 * VoiceManager is compiled into the host target for real, so the voice system's
 * enabled flag and its active-voice count are genuine observations, not stubs.
 *
 * The one thing worth stating up front: this category needed NO new C API
 * functions. All 21 already existed, several under names the token matcher
 * refuses to pair (nativeTriggerChordNotes vs wma_voice_trigger_chord). What it
 * did need was a fix on the JNI side — see MultiTouchIgnoresACountBiggerThan...
 * below for the shape of it, though the over-read itself is unreachable from
 * here, since the C API takes a bare pointer and never sees an array length.
 */

#include "support/CApiFixture.h"

#include <gtest/gtest.h>

namespace wma_test {
namespace {

// wma_voice_update_multi_touch's flat layout: [x, y, freq, amp, pressure, id].
constexpr int kTouchStride = 6;

constexpr int kBlockFrames = 256;

class CApiVoiceTest : public CApiFixture {
protected:
    /**
     * Bring the engine up and enable polyphony.
     *
     * Rendering is not optional here. wma_voice_update_multi_touch only hands
     * the touches to the trigger source; the voices themselves are allocated in
     * VoiceManager::processSourceEvents(), which runs on the audio thread. So
     * the active count does not move until a block goes through — a test that
     * skipped this would read 0 forever and look like a broken API.
     */
    void startWithVoices() {
        startAt(48000, 0);
        wma_voice_enable(mWma, true);
        render(1, kBlockFrames);
    }
};

/// Build @p touches worth of flat touch data, each at a distinct frequency.
std::vector<float> touchData(int touches) {
    std::vector<float> out(static_cast<size_t>(touches) * kTouchStride, 0.0f);
    for (int i = 0; i < touches; ++i) {
        const size_t o = static_cast<size_t>(i) * kTouchStride;
        out[o + 0] = 0.5f;                                  // x
        out[o + 1] = 0.5f;                                  // y
        out[o + 2] = 220.0f * static_cast<float>(i + 1);    // frequency
        out[o + 3] = 0.8f;                                  // amplitude
        out[o + 4] = 1.0f;                                  // pressure
        out[o + 5] = static_cast<float>(i);                 // pointerId
    }
    return out;
}

// ===========================================================================
// Section 14 — voice system
// ===========================================================================

TEST_F(CApiVoiceTest, TheVoiceSystemEnabledFlagRoundTrips) {
    EXPECT_FALSE(wma_voice_is_enabled(mWma));

    wma_voice_enable(mWma, true);
    EXPECT_TRUE(wma_voice_is_enabled(mWma));

    wma_voice_enable(mWma, false);
    EXPECT_FALSE(wma_voice_is_enabled(mWma));
}

TEST_F(CApiVoiceTest, MultiTouchActivatesOneVoicePerTouch) {
    startWithVoices();
    ASSERT_EQ(wma_voice_get_active_count(mWma), 0);

    const auto three = touchData(3);
    wma_voice_update_multi_touch(mWma, three.data(), 3);
    render(1, kBlockFrames);

    EXPECT_EQ(wma_voice_get_active_count(mWma), 3);
}

TEST_F(CApiVoiceTest, LiftingEveryFingerEventuallyFreesTheVoices) {
    startWithVoices();
    const auto three = touchData(3);
    wma_voice_update_multi_touch(mWma, three.data(), 3);
    render(1, kBlockFrames);
    ASSERT_EQ(wma_voice_get_active_count(mWma), 3);

    // The documented way to say "no touches" is a null pointer with count 0,
    // which is what the JNI forwards when Kotlin hands it an empty array.
    wma_voice_update_multi_touch(mWma, nullptr, 0);

    // A lifted voice is NOT free on the next block: it runs its release tail and
    // keeps counting as active while it does, which is correct — it is still
    // making sound. So the assertion is that it eventually frees, not that it
    // frees at once. The bound is deliberately generous (~2.7 s at 48 kHz);
    // tightening it would be pinning the envelope length, which is not what this
    // suite is about.
    int remaining = wma_voice_get_active_count(mWma);
    for (int i = 0; i < 500 && remaining > 0; ++i) {
        render(1, kBlockFrames);
        remaining = wma_voice_get_active_count(mWma);
    }

    EXPECT_EQ(remaining, 0);
}

TEST_F(CApiVoiceTest, MultiTouchCapsAtFourVoicesRegardlessOfTheCount) {
    startWithVoices();

    // The cap is in the unpack loop, and it is the reason the JNI can hand the
    // array straight through: whatever the caller claims, at most 4 are read.
    const auto six = touchData(6);
    wma_voice_update_multi_touch(mWma, six.data(), 6);
    render(1, kBlockFrames);

    EXPECT_EQ(wma_voice_get_active_count(mWma), 4);
}

TEST_F(CApiVoiceTest, MultiTouchIsIgnoredWhileTheVoiceSystemIsOff) {
    ASSERT_FALSE(wma_voice_is_enabled(mWma));

    startAt(48000, 0);
    const auto three = touchData(3);
    wma_voice_update_multi_touch(mWma, three.data(), 3);
    render(1, kBlockFrames);

    EXPECT_EQ(wma_voice_get_active_count(mWma), 0);
}

TEST_F(CApiVoiceTest, DisablingTheVoiceSystemReleasesWhatWasSounding) {
    startWithVoices();
    const auto two = touchData(2);
    wma_voice_update_multi_touch(mWma, two.data(), 2);
    render(1, kBlockFrames);
    ASSERT_EQ(wma_voice_get_active_count(mWma), 2);

    // Otherwise the notes hang: the voices keep sounding with nothing left
    // driving them.
    wma_voice_enable(mWma, false);

    EXPECT_EQ(wma_voice_get_active_count(mWma), 0);
}

TEST_F(CApiVoiceTest, ANullPointerWinsOverANonZeroCountInsteadOfBeingIndexed) {
    // Each degenerate input starts from silence on purpose: once a voice has
    // sounded it keeps counting through its release tail, and a shared setup
    // would leave the assertions measuring that instead of the guard.
    startWithVoices();
    ASSERT_EQ(wma_voice_get_active_count(mWma), 0);

    wma_voice_update_multi_touch(mWma, nullptr, 3);
    render(1, kBlockFrames);

    EXPECT_EQ(wma_voice_get_active_count(mWma), 0);
}

TEST_F(CApiVoiceTest, AZeroCountIsTreatedAsNoTouchesEvenWithARealArray) {
    startWithVoices();
    const auto three = touchData(3);

    wma_voice_update_multi_touch(mWma, three.data(), 0);
    render(1, kBlockFrames);

    EXPECT_EQ(wma_voice_get_active_count(mWma), 0);
}

TEST_F(CApiVoiceTest, OnlyTheFirstCountTouchesOfTheArrayAreRead) {
    startWithVoices();
    const auto three = touchData(3);

    // The array holds three, the caller says two: two voices, not three. This is
    // the C API side of the over-read the JNI now guards against — from here the
    // count can only be smaller than the array, never larger, because a bare
    // pointer carries no length.
    wma_voice_update_multi_touch(mWma, three.data(), 2);
    render(1, kBlockFrames);

    EXPECT_EQ(wma_voice_get_active_count(mWma), 2);
}

TEST_F(CApiVoiceTest, TheChordCallsAreSurvivableAndReleaseCleanly) {
    // Chord voices do not surface through getActiveVoiceCount (they run through
    // ChordGenerator, not VoiceManager), so this is a smoke test and says so.
    const float triad[] = {261.63f, 329.63f, 392.00f};

    wma_voice_trigger_chord(mWma, triad, 3, 0.8f, 0);
    wma_voice_update_chord(mWma, triad, 3, 0.5f);
    wma_voice_release_chord(mWma);

    // Degenerate inputs must be rejected rather than indexed.
    wma_voice_trigger_chord(mWma, nullptr, 3, 0.8f, 0);
    wma_voice_trigger_chord(mWma, triad, 0, 0.8f, 0);
    wma_voice_trigger_chord(mWma, triad, -1, 0.8f, 0);
    wma_voice_update_chord(mWma, nullptr, 3, 0.5f);
    wma_voice_update_chord(mWma, triad, 0, 0.5f);

    EXPECT_TRUE(wma_is_initialized(mWma));
}

TEST_F(CApiVoiceTest, SettingTheVoiceCeilingCurrentlyDoesNothing) {
    // NOT the behaviour this should have. VoiceManager::setMaxVoices clamps its
    // argument, logs, and returns — the log line says so out loud: "requires
    // recreation of VoicePool to take effect". So wma_voice_set_max, and
    // nativeSetMaxVoices behind it, are a public setter that changes nothing
    // while still bumping the state version.
    //
    // This test exists to say that, not to bless it. Fixing it means recreating
    // the pool, which is voice-allocation surgery and does not belong in a
    // migration PR — it is registered as its own item. If someone does fix it,
    // this test fails, which is the correct way to find out.
    startAt(48000, 0);
    wma_voice_set_max(mWma, 2);
    wma_voice_set_stealing_strategy(mWma, 1);
    wma_voice_enable(mWma, true);
    render(1, kBlockFrames);

    const auto three = touchData(3);
    wma_voice_update_multi_touch(mWma, three.data(), 3);
    render(1, kBlockFrames);

    EXPECT_EQ(wma_voice_get_active_count(mWma), 3)
        << "if this now reads 2, setMaxVoices was implemented — update this test";
}

// ===========================================================================
// Sections 7 and 13 — voice filter and dual touch
// ===========================================================================

TEST_F(CApiVoiceTest, TheDualTouchModeFlagRoundTrips) {
    EXPECT_FALSE(wma_get_dual_touch_mode(mWma));

    wma_set_dual_touch_mode(mWma, true);
    EXPECT_TRUE(wma_get_dual_touch_mode(mWma));

    wma_set_dual_touch_mode(mWma, false);
    EXPECT_FALSE(wma_get_dual_touch_mode(mWma));
}

TEST_F(CApiVoiceTest, TheVoiceFilterAndDualTouchSettersAreSurvivable) {
    // No getters for these either. The mix-mode range check moved out of the
    // JNI into wma_set_dual_touch_mix_mode and shares that fate — an out-of-
    // range id is indistinguishable from an accepted one from out here.
    wma_voice_filter_set_enabled(mWma, true);
    wma_voice_filter_set_cutoff(mWma, 800.0f);
    wma_voice_filter_set_resonance(mWma, 0.7f);
    wma_voice_filter_set_mode(mWma, 1);

    wma_set_dual_touch(mWma, 0.1f, 0.2f, 220.0f, 0.5f, 1.0f,
                             0.8f, 0.9f, 440.0f, 0.5f, 1.0f,
                             0.7f, 0.5f);
    wma_set_dual_touch_mix_mode(mWma, 3);
    wma_set_dual_touch_mix_mode(mWma, 99);   // out of range, rejected
    wma_set_dual_touch_mix_mode(mWma, -1);   // out of range, rejected

    EXPECT_TRUE(wma_is_initialized(mWma));
}

// ===========================================================================
// Section 6 — SoundFont notes
// ===========================================================================

TEST_F(CApiVoiceTest, TheSoundFontNoteCallsAreSurvivableWithNoSoundFontLoaded) {
    // No .sf2 fixture yet (registered debt), so what is checked here is that
    // the note path does not fall over when the engine has nothing loaded —
    // which is exactly the state a user is in before picking an instrument.
    ASSERT_FALSE(wma_sf_is_loaded(mWma));

    wma_sf_note_on(mWma, 0, 60, 0.8f);
    wma_sf_note_on(mWma, 1, 64, 0.8f);
    wma_sf_note_off(mWma, 0);
    wma_sf_note_off_all_except(mWma, 1);
    wma_sf_note_off_all(mWma);

    EXPECT_TRUE(wma_is_initialized(mWma));
}

// ===========================================================================
// Null handle
// ===========================================================================

TEST(CApiVoiceNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    EXPECT_FALSE(wma_voice_is_enabled(nullptr));
    EXPECT_EQ(wma_voice_get_active_count(nullptr), 0);
    EXPECT_FALSE(wma_get_dual_touch_mode(nullptr));
}

TEST(CApiVoiceNullHandle, EveryMutatorIsANoOpRatherThanACrash) {
    const float triad[] = {261.63f, 329.63f, 392.00f};
    const std::vector<float> touches(6 * kTouchStride, 0.5f);

    wma_voice_enable(nullptr, true);
    wma_voice_update_multi_touch(nullptr, touches.data(), 4);
    wma_voice_set_max(nullptr, 4);
    wma_voice_set_stealing_strategy(nullptr, 1);
    wma_voice_trigger_chord(nullptr, triad, 3, 0.8f, 0);
    wma_voice_update_chord(nullptr, triad, 3, 0.5f);
    wma_voice_release_chord(nullptr);

    wma_voice_filter_set_enabled(nullptr, true);
    wma_voice_filter_set_cutoff(nullptr, 800.0f);
    wma_voice_filter_set_resonance(nullptr, 0.7f);
    wma_voice_filter_set_mode(nullptr, 1);

    wma_set_dual_touch_mode(nullptr, true);
    wma_set_dual_touch(nullptr, 0, 0, 220.0f, 0.5f, 1.0f,
                                0, 0, 440.0f, 0.5f, 1.0f, 0.5f, 0.5f);
    wma_set_dual_touch_mix_mode(nullptr, 2);

    wma_sf_note_on(nullptr, 0, 60, 0.8f);
    wma_sf_note_off(nullptr, 0);
    wma_sf_note_off_all(nullptr);
    wma_sf_note_off_all_except(nullptr, 1);
    SUCCEED();
}

}  // namespace
}  // namespace wma_test
