/**
 * test_fade_lifecycle.cpp
 *
 * stopWithFade / pauseWithFade / resumeWithFade on the BackendManager path.
 *
 * All three used to branch on `if (mStream)`, and mStream is permanently null
 * once the engine runs through BackendManager. stopWithFade therefore fell
 * through to a bare stop() — the audio was cut dead, an audible click — while
 * pause and resume snapped the volume instead of ramping it. Only the legacy
 * Oboe path ever got a fade.
 *
 * The fade ramp is advanced by the render callback (applyEffectsAndOutput
 * pulls one block off FadeController), so these tests start the engine over a
 * fake backend and drive onAudioReady() by hand. That makes the assertions
 * about the *shape* of the ramp, not just about a flag having been set —
 * including the one that proves the ramp length comes from the rate the device
 * negotiated rather than the rate the app asked for.
 */

#include "support/BackendPathFixture.h"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

using FadeLifecycleTest = BackendPathFixture;

constexpr int kBlockFrames = 256;
constexpr int kBlocks = 4;
constexpr int kRenderedFrames = kBlockFrames * kBlocks;

// Engine states, mirroring EngineState in AudioEngine.h.
constexpr int kStateStopped = 0;
constexpr int kStateRunning = 2;

/// Bring the engine to full volume so a following fade-out starts from 1.0.
void settleAtFullVolume(AudioEngine& engine) {
    // start() arms a 0 → 1 ramp with zero length; one block completes it.
    std::vector<float> buffer(kBlockFrames * 2, 0.0f);
    engine.onAudioReady(buffer.data(), nullptr, kBlockFrames);
}

// ===========================================================================
// stopWithFade
// ===========================================================================

TEST_F(FadeLifecycleTest, StopWithFadeRampsDownInsteadOfCuttingTheAudio) {
    constexpr int kFadeMs = 100;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    ASSERT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);

    mEngine->stopWithFade(kFadeMs);

    // Before the fix this path armed no ramp at all: it called stop() straight
    // away, so there was nothing to fade and the volume jumped to zero.
    EXPECT_TRUE(mEngine->getIsFading());
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 0.0f);
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);

    render(kBlocks, kBlockFrames);

    // 1024 of the 4800 frames the ramp spans at 48 kHz.
    const float expected = 1.0f - static_cast<float>(kRenderedFrames) / 4800.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expected, 0.01f);

    awaitDetachedStop(kFadeMs);
}

TEST_F(FadeLifecycleTest, StopWithFadeSpansTheNegotiatedRateNotThePreferredOne) {
    constexpr int kFadeMs = 100;
    // The device settles at half the rate the app wanted. A ramp measured
    // against the preferred rate would last twice as many frames as it should,
    // so the audio would still be clearly audible when the stream is torn down.
    startEngineAt(24000);
    mEngine->setPreferredSampleRate(48000);
    settleAtFullVolume(*mEngine);
    ASSERT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);

    mEngine->stopWithFade(kFadeMs);
    render(kBlocks, kBlockFrames);

    const float expectedAtNegotiatedRate = 1.0f - static_cast<float>(kRenderedFrames) / 2400.0f;
    const float expectedAtPreferredRate = 1.0f - static_cast<float>(kRenderedFrames) / 4800.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expectedAtNegotiatedRate, 0.01f);
    EXPECT_GT(std::abs(expectedAtNegotiatedRate - expectedAtPreferredRate), 0.1f)
        << "the two rates must give visibly different ramps for this test to mean anything";

    awaitDetachedStop(kFadeMs);
}

TEST_F(FadeLifecycleTest, StopWithFadeStopsImmediatelyWhenNoFadeTimeIsGiven) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    ASSERT_EQ(mEngine->getEngineState(), kStateRunning);

    mEngine->stopWithFade(0);

    // No ramp, no detached thread — a synchronous stop, which is what a caller
    // asking for a zero-length fade means.
    EXPECT_EQ(mEngine->getEngineState(), kStateStopped);
    EXPECT_FALSE(mEngine->getIsFading());
}

TEST_F(FadeLifecycleTest, StopWithFadeEventuallyStopsTheEngine) {
    constexpr int kFadeMs = 20;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    ASSERT_EQ(mEngine->getEngineState(), kStateRunning);

    mEngine->stopWithFade(kFadeMs);
    ASSERT_EQ(mEngine->getEngineState(), kStateRunning) << "the stop must trail the fade";

    awaitDetachedStop(kFadeMs);

    EXPECT_EQ(mEngine->getEngineState(), kStateStopped);
}

// ===========================================================================
// pauseWithFade / resumeWithFade
// ===========================================================================

TEST_F(FadeLifecycleTest, PauseWithFadeRampsDownBeforeItPauses) {
    constexpr int kFadeMs = 100;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->pauseWithFade(kFadeMs);

    // The old backend branch set paused=true on the spot; the pause is supposed
    // to land only once the ramp has run.
    EXPECT_FALSE(mEngine->getIsPaused());
    EXPECT_TRUE(mEngine->getIsFading());
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 0.0f);
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);

    render(kBlocks, kBlockFrames);
    const float expected = 1.0f - static_cast<float>(kRenderedFrames) / 4800.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expected, 0.01f);
}

TEST_F(FadeLifecycleTest, PauseWithFadeLandsOnPausedAfterTheRamp) {
    constexpr int kFadeMs = 30;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->pauseWithFade(kFadeMs);
    ASSERT_FALSE(mEngine->getIsPaused());

    // FadeController arms the pause for fadeTimeMs + 50; the margin here is for
    // scheduling, not for the fade.
    std::this_thread::sleep_for(std::chrono::milliseconds(kFadeMs + 400));

    EXPECT_TRUE(mEngine->getIsPaused());
}

TEST_F(FadeLifecycleTest, PauseWithFadePausesImmediatelyWhenNoFadeTimeIsGiven) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->pauseWithFade(0);

    EXPECT_TRUE(mEngine->getIsPaused());
    EXPECT_FALSE(mEngine->getIsFading());
}

TEST_F(FadeLifecycleTest, ResumeWithFadeRampsUpFromSilence) {
    constexpr int kFadeMs = 100;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    mEngine->pauseWithFade(0);
    ASSERT_TRUE(mEngine->getIsPaused());

    mEngine->resumeWithFade(kFadeMs);

    // The old backend branch cleared the pause and jumped straight back to 1.0.
    EXPECT_FALSE(mEngine->getIsPaused());
    EXPECT_TRUE(mEngine->getIsFading());
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 0.0f);
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 1.0f);

    render(kBlocks, kBlockFrames);
    const float expected = static_cast<float>(kRenderedFrames) / 4800.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expected, 0.01f);
}

TEST_F(FadeLifecycleTest, ResumeWithFadeSpansTheNegotiatedRateNotThePreferredOne) {
    constexpr int kFadeMs = 100;
    startEngineAt(24000);
    mEngine->setPreferredSampleRate(48000);
    settleAtFullVolume(*mEngine);
    mEngine->pauseWithFade(0);

    mEngine->resumeWithFade(kFadeMs);
    render(kBlocks, kBlockFrames);

    const float expectedAtNegotiatedRate = static_cast<float>(kRenderedFrames) / 2400.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expectedAtNegotiatedRate, 0.01f);
}

TEST_F(FadeLifecycleTest, ResumeWithFadeRestoresFullVolumeWhenNoFadeTimeIsGiven) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    mEngine->pauseWithFade(0);
    ASSERT_TRUE(mEngine->getIsPaused());

    mEngine->resumeWithFade(0);

    // Zero-length resume must land on 1.0, not on 0.0 — a resume that leaves
    // the fade volume at zero is silence the user cannot get out of.
    EXPECT_FALSE(mEngine->getIsPaused());
    EXPECT_FALSE(mEngine->getIsFading());
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 1.0f);
}

TEST_F(FadeLifecycleTest, ResumeWithFadeCancelsAPendingPause) {
    constexpr int kPauseFadeMs = 30;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->pauseWithFade(kPauseFadeMs);
    mEngine->resumeWithFade(kPauseFadeMs);

    // The pause was armed on a timer. If resume did not cancel it, it would
    // fire behind the resume and leave the engine silently paused.
    std::this_thread::sleep_for(std::chrono::milliseconds(kPauseFadeMs + 400));

    EXPECT_FALSE(mEngine->getIsPaused());
}

// ===========================================================================
// Fade output, not just fade state
// ===========================================================================

TEST_F(FadeLifecycleTest, PauseSilencesTheRenderedBlock) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    render(4, kBlockFrames);

    // A paused engine renders silence wherever the ramp happened to stop:
    // applyEffectsAndOutput pins both ends of the gain ramp to zero.
    mEngine->pauseWithFade(0);
    ASSERT_TRUE(mEngine->getIsPaused());

    // Not bit-exact zero, and legitimately so — the output stage's DC blocker
    // is a filter with memory, so it decays from the pre-pause signal rather
    // than snapping. One block of settling puts the tail well below -60 dBFS.
    render(1, kBlockFrames);

    std::vector<float> buffer(kBlockFrames * 2, 1.0f);
    mEngine->onAudioReady(buffer.data(), nullptr, kBlockFrames);

    float peak = 0.0f;
    for (float sample : buffer) {
        peak = std::max(peak, std::abs(sample));
    }
    EXPECT_LT(peak, 1.0e-3f);
}

}  // namespace
}  // namespace wma_test
