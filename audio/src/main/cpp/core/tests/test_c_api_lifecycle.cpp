/**
 * test_c_api_lifecycle.cpp
 *
 * The lifecycle / state / volume surface of the C API (sections 1–3 of
 * watermelon_audio.h), covered from the C API itself rather than from
 * AudioEngine.
 *
 * WA-2.6 turned the matching 22 JNI entry points into one-line calls into these
 * functions, which moved two things out of jni_audio_bridge.cpp and into here:
 *
 *   - the value every query returns when there is no engine yet. The JNI used
 *     to spell those out by hand (`if (!g_jniState.engine) return 0.0f;`) and
 *     now leans on the WMA_CHECK macros. NullHandle* below is that contract.
 *
 *   - the difference between "start with the engine's own fade" and "start with
 *     an explicit fade of N ms", which the JNI has always had as two separate
 *     entry points and the C API used to collapse at N = 0. FadeDefault* below
 *     is that distinction — it is also the bug this category fixed: the iOS
 *     bridge mapped both onto fade_time_ms = 0 and silently got the default for
 *     both, so the same call did different things on the two platforms.
 *
 * The ramp is advanced by the render callback, so the fade tests drive
 * onAudioReady() by hand. That is the one thing reached through the internal
 * header: the C API has no "render a block" function and should not grow one.
 */

#include "support/CApiFixture.h"

#include <gtest/gtest.h>

namespace wma_test {
namespace {

// Engine states, mirroring EngineState in AudioEngine.h.
constexpr int kStateStopped = 0;
constexpr int kStateRunning = 2;

// AudioEngine::start() declares `int fadeTimeMs = 10`. That default is the
// whole point of WMA_FADE_DEFAULT: it is a real ramp, not an instant start.
constexpr int kEngineDefaultFadeMs = 10;

// startAt() and render() live in CApiFixture — three suites need them now.
using CApiLifecycleTest = CApiFixture;

// ===========================================================================
// WMA_FADE_DEFAULT vs an explicit fade length
//
// Both used to reach start(), so a caller asking for a hard cut got the 10 ms
// default instead. 64 frames at 48 kHz is well inside a 480-frame ramp and well
// past a zero-length one, which is what makes the two cases tell apart.
// ===========================================================================

constexpr int kProbeFrames = 64;

TEST_F(CApiLifecycleTest, FadeDefaultStartArmsTheEnginesOwnRamp) {
    startAt(48000, WMA_FADE_DEFAULT);
    render(1, kProbeFrames);

    EXPECT_TRUE(wma_is_fading(mWma))
        << "WMA_FADE_DEFAULT must leave the engine's default ramp running";
    const float v = wma_get_fade_volume(mWma);
    EXPECT_GT(v, 0.0f);
    EXPECT_LT(v, 1.0f);
}

TEST_F(CApiLifecycleTest, FadeDefaultStartRampIsShortAndCompletes) {
    startAt(48000, WMA_FADE_DEFAULT);

    // 10 ms at 48 kHz is 480 frames; one extra block clears any rounding.
    const int rampFrames = kEngineDefaultFadeMs * 48000 / 1000;
    render(1, rampFrames + kProbeFrames);

    EXPECT_FALSE(wma_is_fading(mWma));
    EXPECT_FLOAT_EQ(wma_get_fade_volume(mWma), 1.0f);
}

TEST_F(CApiLifecycleTest, AnExplicitZeroFadeStartsAtFullVolumeInsteadOfRamping) {
    startAt(48000, 0);
    render(1, kProbeFrames);

    // This is the case that used to be swallowed: fade_time_ms = 0 meant
    // "no preference" and got the 10 ms default, so it was still ramping here.
    EXPECT_FALSE(wma_is_fading(mWma));
    EXPECT_FLOAT_EQ(wma_get_fade_volume(mWma), 1.0f);
}

TEST_F(CApiLifecycleTest, FadeDefaultStopDoesNotArmARamp) {
    startAt(48000, 0);
    render(1, kProbeFrames);
    ASSERT_EQ(wma_get_engine_state(mWma), kStateRunning);

    EXPECT_EQ(wma_engine_stop(mWma, WMA_FADE_DEFAULT), WMA_OK);

    // A synchronous stop, which is what nativeStopEngine() has always been.
    EXPECT_EQ(wma_get_engine_state(mWma), kStateStopped);
    EXPECT_FALSE(wma_is_fading(mWma));
}

TEST_F(CApiLifecycleTest, AnExplicitStopFadeRampsDownBeforeItStops) {
    constexpr int kFadeMs = 100;
    startAt(48000, 0);
    render(1, kProbeFrames);
    ASSERT_FLOAT_EQ(wma_get_fade_volume(mWma), 1.0f);

    EXPECT_EQ(wma_engine_stop(mWma, kFadeMs), WMA_OK);

    EXPECT_TRUE(wma_is_fading(mWma));
    EXPECT_FLOAT_EQ(wma_get_target_fade_volume(mWma), 0.0f);
    EXPECT_EQ(wma_get_engine_state(mWma), kStateRunning)
        << "the stop must trail the fade, not precede it";
}

TEST_F(CApiLifecycleTest, AnExplicitZeroStopFadeStopsImmediately) {
    startAt(48000, 0);
    render(1, kProbeFrames);

    EXPECT_EQ(wma_engine_stop(mWma, 0), WMA_OK);

    EXPECT_EQ(wma_get_engine_state(mWma), kStateStopped);
    EXPECT_FALSE(wma_is_fading(mWma));
}

// ===========================================================================
// Pause / resume
// ===========================================================================

TEST_F(CApiLifecycleTest, PauseAndResumeWithoutAFadeFlipIsPausedOnTheSpot) {
    startAt(48000, 0);
    render(1, kProbeFrames);
    ASSERT_FALSE(wma_is_paused(mWma));

    EXPECT_EQ(wma_engine_pause(mWma, 0), WMA_OK);
    EXPECT_TRUE(wma_is_paused(mWma));

    EXPECT_EQ(wma_engine_resume(mWma, 0), WMA_OK);
    EXPECT_FALSE(wma_is_paused(mWma));
}

// ===========================================================================
// State and volume queries the JNI now delegates
// ===========================================================================

TEST_F(CApiLifecycleTest, MasterVolumeIsClampedToTheUnitRange) {
    // The clamp used to be written out again in nativeSetMasterVolume.
    wma_set_master_volume(mWma, 1.5f);
    EXPECT_FLOAT_EQ(wma_get_master_volume(mWma), 1.0f);

    wma_set_master_volume(mWma, -0.5f);
    EXPECT_FLOAT_EQ(wma_get_master_volume(mWma), 0.0f);

    wma_set_master_volume(mWma, 0.25f);
    EXPECT_FLOAT_EQ(wma_get_master_volume(mWma), 0.25f);
}

// ===========================================================================
// Synth volume — the instrument level (synth + FX, not the loops)
// ===========================================================================

TEST_F(CApiLifecycleTest, SynthVolumeDefaultsToUnityAndIsClamped) {
    // The default matters more than it looks. This gain multiplies the fade ramp
    // on every block of every mode, so anything other than 1.0 here quietly
    // changes how the engine has always sounded. If this ever fails, the sound
    // of the product changed — that is the finding, not the number.
    EXPECT_FLOAT_EQ(wma_get_synth_volume(mWma), 1.0f);

    wma_set_synth_volume(mWma, 1.5f);
    EXPECT_FLOAT_EQ(wma_get_synth_volume(mWma), 1.0f);

    wma_set_synth_volume(mWma, -0.5f);
    EXPECT_FLOAT_EQ(wma_get_synth_volume(mWma), 0.0f);

    wma_set_synth_volume(mWma, 0.25f);
    EXPECT_FLOAT_EQ(wma_get_synth_volume(mWma), 0.25f);
}

TEST_F(CApiLifecycleTest, SynthVolumeActuallyScalesTheAudibleOutput) {
    // The point of this one is that it reads the BUFFER, not the setter. A
    // get/set round-trip would pass just as happily with the gain never applied
    // — which is exactly how setMaxVoices and the output meters stayed "green"
    // while doing nothing.
    constexpr int kFrames = 256;

    auto peakOver = [&](int blocks) {
        float peak = 0.0f;
        for (int i = 0; i < blocks; ++i) {
            peak = std::max(peak, renderBlockPeak(kFrames));
        }
        return peak;
    };

    startAt(48000, 0);

    // 0.3 and not 1.0, and this is the whole reason the first version of this
    // test failed. OutputStage::processOutput runs a lookahead limiter and a
    // soft clipper, so near full scale the chain is deliberately NON-LINEAR: at
    // amplitude 1.0 the block peaked at 0.84 and halving the gain gave 0.47, a
    // ratio of 0.56. Nothing was wrong with the gain — the test was measuring
    // the limiter. Staying well under the threshold keeps the path linear, which
    // is the only regime where "half in, half out" is a true statement.
    wma_set_frequency_amplitude(mWma, 440.0f, 0.3f);

    // Settle past the start ramp so the fade is out of the picture and the only
    // thing scaling the block is the synth volume.
    render(20, kFrames);
    const float atUnity = peakOver(8);
    ASSERT_GT(atUnity, 0.01f)
        << "the oscillator has to be audible for the ratio below to mean anything";

    wma_set_synth_volume(mWma, 0.5f);
    render(4, kFrames);  // let the inter-block ramp reach the new value
    const float atHalf = peakOver(8);

    EXPECT_NEAR(atHalf, atUnity * 0.5f, atUnity * 0.05f)
        << "halving the instrument level should halve what comes out";

    wma_set_synth_volume(mWma, 0.0f);
    render(4, kFrames);
    EXPECT_LT(peakOver(4), 0.001f) << "zero has to be silence";
}

TEST_F(CApiLifecycleTest, StreamInfoReportsTheNegotiatedRateAndFillsEveryOutParam) {
    startAt(44100, 0);

    int sampleRate = -1, bufferSize = -1;
    float latencyMs = -1.0f;
    ASSERT_TRUE(wma_get_stream_info(mWma, &sampleRate, &bufferSize, &latencyMs));

    // nativeGetStreamInfo packs these three into a float[3] in this order.
    EXPECT_EQ(sampleRate, 44100);
    EXPECT_GE(bufferSize, 0);
    EXPECT_GE(latencyMs, 0.0f);
}

TEST_F(CApiLifecycleTest, StreamInfoReportsNoStreamBeforeTheEngineStarts) {
    int sampleRate = -1, bufferSize = -1;
    float latencyMs = -1.0f;
    // nativeGetStreamInfo returns null to Kotlin in exactly this case.
    EXPECT_FALSE(wma_get_stream_info(mWma, &sampleRate, &bufferSize, &latencyMs));
}

TEST_F(CApiLifecycleTest, AFreshEngineIsInitializedAndClean) {
    EXPECT_TRUE(wma_is_initialized(mWma));
    EXPECT_FALSE(wma_has_init_failed(mWma));
    EXPECT_FALSE(wma_has_error(mWma));
    EXPECT_EQ(wma_get_last_error_code(mWma), 0);
    EXPECT_EQ(wma_get_engine_state(mWma), kStateStopped);
}

// ===========================================================================
// The null-handle contract
//
// This is what jni_audio_bridge.cpp now relies on: every one of its hand-written
// `if (!g_jniState.engine) return <default>;` guards was deleted in favour of
// these. The values below are the values that file used to return.
// ===========================================================================

TEST(CApiNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    EXPECT_EQ(wma_get_engine_state(nullptr), 0);
    EXPECT_FALSE(wma_is_paused(nullptr));
    EXPECT_EQ(wma_get_state_version(nullptr), 0u);
    EXPECT_FALSE(wma_has_error(nullptr));
    EXPECT_EQ(wma_get_last_error_code(nullptr), 0);
    EXPECT_FALSE(wma_has_init_failed(nullptr));
    EXPECT_FALSE(wma_is_initialized(nullptr));
    EXPECT_FALSE(wma_is_using_reduced_buffers(nullptr));

    EXPECT_FLOAT_EQ(wma_get_fade_volume(nullptr), 0.0f);
    EXPECT_FLOAT_EQ(wma_get_target_fade_volume(nullptr), 0.0f);
    EXPECT_FALSE(wma_is_fading(nullptr));
    EXPECT_FLOAT_EQ(wma_get_fade_progress(nullptr), 0.0f);

    // nativeGetMasterVolume returned 1.0f, not 0.0f — a missing engine is not
    // the same as a muted one. Same reasoning for the synth level.
    EXPECT_FLOAT_EQ(wma_get_master_volume(nullptr), 1.0f);
    EXPECT_FLOAT_EQ(wma_get_synth_volume(nullptr), 1.0f);
    wma_set_synth_volume(nullptr, 0.5f);  // must not crash

    int sampleRate = -1, bufferSize = -1;
    float latencyMs = -1.0f;
    EXPECT_FALSE(wma_get_stream_info(nullptr, &sampleRate, &bufferSize, &latencyMs));
}

TEST(CApiNullHandle, EveryMutatorIsANoOpRatherThanACrash) {
    EXPECT_EQ(wma_engine_start(nullptr, WMA_FADE_DEFAULT), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_engine_start(nullptr, 100), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_engine_stop(nullptr, WMA_FADE_DEFAULT), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_engine_stop(nullptr, 100), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_engine_pause(nullptr, 0), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_engine_resume(nullptr, 0), WMA_ERROR_NOT_INITIALIZED);

    // Void returns: the assertion is that these come back at all.
    wma_clear_error(nullptr);
    wma_set_master_volume(nullptr, 0.5f);
    wma_engine_destroy(nullptr);
    SUCCEED();
}

}  // namespace
}  // namespace wma_test
