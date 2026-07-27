/**
 * test_c_api_looper.cpp
 *
 * The looper — section 19 of watermelon_audio.h. Batch 1 of the category: the
 * 40 entry points that already had a `wma_looper_*` counterpart and were still
 * transcribing AudioLooper by hand in the JNI.
 *
 * WHAT THIS FILE IS AND IS NOT FOR.
 *
 * AudioLooper's own behaviour is already covered, and thoroughly, by
 * looper/tests/ — test_audio_looper.cpp, test_track_buffer.cpp,
 * test_free_loop_autosync.cpp and friends. Re-asserting "recording makes a
 * track active" here would be duplicating a better test one directory over.
 *
 * What was NOT covered, and is what this batch actually changed, is the C API
 * boundary:
 *
 *   - 38 hand-written "no engine" defaults moved out of the JNI and into
 *     wma_looper_*. If any one of them changed value in the move, Android
 *     changes behaviour silently and no compiler says a word. The null-handle
 *     sweep below pins every single one against what the JNI used to return.
 *
 *   - The C API has to reach THE SAME AudioLooper the audio callback renders.
 *     A wma_* that quietly operated on a different instance would pass every
 *     state round-trip and still be silent in production — that is exactly the
 *     bug the InputNode unification fixed, so it is worth proving rather than
 *     assuming. AudibleRoundTrip below records real synth output through the C
 *     API and plays it back.
 *
 *   - wma_looper_set_track_loop_region took `int` while every other link in the
 *     chain —Kotlin Long, jlong, AudioLooper's int64_t— was 64-bit. See below.
 */

#include "support/CApiFixture.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kBlockFrames = 256;
constexpr int kSampleRate = 48000;

/// Anything above this is the engine making sound rather than the dither floor.
constexpr float kAudible = 0.01f;

class CApiLooperTest : public CApiFixture {
protected:
    /// Record real engine output into @p track, so it has content and a length.
    ///
    /// Not a convenience: a track that was only prepare()d has capacity but
    /// mLengthFrames == 0, and TrackBuffer::setLoopRegion returns early on a
    /// zero length. A loop region only exists on a track that has something to
    /// loop, which is worth knowing before writing a test that assumes
    /// otherwise.
    void recordTrack(int track, int blocks = 4) {
        wma_looper_set_enabled(mWma, true);
        ASSERT_EQ(wma_looper_prepare_track(mWma, track, blocks * kBlockFrames, kSampleRate),
                  WMA_OK);
        wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
        render(4, kBlockFrames);
        wma_looper_start_recording(mWma, track);
        render(blocks, kBlockFrames);
        wma_looper_stop_recording(mWma);
        wma_set_frequency_amplitude(mWma, 440.0f, 0.0f);
        ASSERT_GT(wma_looper_get_track_length_frames(mWma, track), 0);
    }
};

// ===========================================================================
// The C API drives the looper the engine actually renders
// ===========================================================================

TEST_F(CApiLooperTest, RecordingThroughTheCApiCapturesTheEngineOutput) {
    startAt(kSampleRate, /*fadeTimeMs=*/0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 4 * kBlockFrames, kSampleRate), WMA_OK);

    // Make the engine produce something worth recording.
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(4, kBlockFrames);
    ASSERT_GT(renderBlockPeak(kBlockFrames), kAudible)
        << "the synth is silent, so this test cannot tell recording from nothing";

    wma_looper_start_recording(mWma, 0);
    EXPECT_TRUE(wma_looper_is_recording(mWma));
    render(4, kBlockFrames);
    wma_looper_stop_recording(mWma);

    EXPECT_FALSE(wma_looper_is_recording(mWma));
    EXPECT_TRUE(wma_looper_is_track_active(mWma, 0))
        << "a track recorded through the C API should be active";
    EXPECT_GT(wma_looper_get_track_length_frames(mWma, 0), 0);
}

TEST_F(CApiLooperTest, AudibleRoundTrip) {
    startAt(kSampleRate, 0);
    recordTrack(0);

    // The first attempt at this test asserted silence right after killing the
    // oscillator, and failed at 0.6 — because the freshly recorded loop was
    // already playing. There is no way to prove "the synth is off" while the
    // thing under test is making noise, so the looper gets paused first. That
    // also puts pause/resume under test, which is a better test either way.
    wma_looper_pause(mWma);
    render(60, kBlockFrames);
    ASSERT_LT(renderBlockPeak(kBlockFrames), kAudible)
        << "with the oscillator silent and the looper paused, nothing should come out";

    // Resume: whatever comes out now is the loop, because nothing else is on.
    wma_looper_resume(mWma);
    float loudest = 0.0f;
    for (int i = 0; i < 12; ++i) {
        loudest = std::max(loudest, renderBlockPeak(kBlockFrames));
    }
    EXPECT_GT(loudest, kAudible)
        << "the recorded loop is not reaching the output — the C API and the audio "
           "callback may be looking at different AudioLooper instances";

    // And muting it through the C API silences that same playback.
    //
    // The mute is SMOOTHED — a one-pole at 0.995/sample, so ~0.28 gain left
    // after the first 256-frame block and ~3e-5 after eight. Measuring straight
    // after the call reads the ramp, not the mute: the first version of this
    // assertion failed at 0.61 for exactly that reason, with the code behaving
    // correctly. Let it settle, then measure.
    wma_looper_set_track_muted(mWma, 0, true);
    render(8, kBlockFrames);

    float mutedLoudest = 0.0f;
    for (int i = 0; i < 8; ++i) {
        mutedLoudest = std::max(mutedLoudest, renderBlockPeak(kBlockFrames));
    }
    EXPECT_LT(mutedLoudest, kAudible) << "muting through the C API did nothing";

    // Unmuting brings it back, so the silence above was the mute and not the
    // loop having simply run out.
    wma_looper_set_track_muted(mWma, 0, false);
    render(8, kBlockFrames);
    float unmutedLoudest = 0.0f;
    for (int i = 0; i < 8; ++i) {
        unmutedLoudest = std::max(unmutedLoudest, renderBlockPeak(kBlockFrames));
    }
    EXPECT_GT(unmutedLoudest, kAudible) << "unmuting did not restore the loop";
}

// ===========================================================================
// The loop region, which is 64-bit everywhere except where it used to be
// ===========================================================================

TEST_F(CApiLooperTest, TheLoopRegionRoundTrips) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/32);   // 8192 frames, room for a 1000..5000 region

    wma_looper_set_track_loop_region(mWma, 0, 1000, 5000);
    EXPECT_EQ(wma_looper_get_track_loop_start(mWma, 0), 1000);
    EXPECT_EQ(wma_looper_get_track_loop_end(mWma, 0), 5000);

    wma_looper_reset_track_loop_region(mWma, 0);
    EXPECT_EQ(wma_looper_get_track_loop_start(mWma, 0), 0);
}

TEST_F(CApiLooperTest, AnOutOfRangeLoopRegionSaturatesInsteadOfWrapping) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/32);

    // THE reason this signature is int64_t. AudioLooper::setTrackLoopRegion takes
    // int64_t and clamps into int32 itself, because TrackBuffer stores frames as
    // int32 while the API has to survive a long recording. The C API used to
    // declare `int`, which made it the one narrow link in a chain that is 64-bit
    // from Kotlin's Long down — the clamp could never run, because the value was
    // already truncated by the time it arrived.
    //
    // 0x1'0000'03E8 is 1000 once truncated to 32 bits. If the narrowing were back,
    // this would set a loop region starting at frame 1000 instead of saturating.
    constexpr int64_t kWrapsTo1000 = (int64_t{1} << 32) | 1000;
    wma_looper_set_track_loop_region(mWma, 0, kWrapsTo1000, kWrapsTo1000 + 4000);

    EXPECT_NE(wma_looper_get_track_loop_start(mWma, 0), 1000)
        << "a 64-bit frame index was truncated to 32 bits somewhere in the chain";
}

// ===========================================================================
// Round-trips through the C API — the setters reach the getters
// ===========================================================================

TEST_F(CApiLooperTest, MasterVolumeRoundTrips) {
    startAt(kSampleRate, 0);
    wma_looper_set_master_volume(mWma, 0.25f);
    EXPECT_FLOAT_EQ(wma_looper_get_master_volume(mWma), 0.25f);
}

TEST_F(CApiLooperTest, TrackSpeedRoundTrips) {
    startAt(kSampleRate, 0);
    recordTrack(0);

    wma_looper_set_track_speed(mWma, 0, 0.5f);
    EXPECT_FLOAT_EQ(wma_looper_get_track_speed(mWma, 0), 0.5f);
}

TEST_F(CApiLooperTest, PreparingATrackReportsMemoryFailureRatherThanSucceeding) {
    startAt(kSampleRate, 0);

    // The JNI used to map this by hand to JniError::MEMORY_ALLOCATION_FAILED.
    // JniError and WmaResult are numerically identical entry for entry, which is
    // what makes the delegation value-preserving for the Kotlin side — its
    // NativeErrorCode enum reads the raw int.
    static_assert(static_cast<int>(WMA_ERROR_MEMORY) == -6,
                  "NativeErrorCode.MEMORY_ALLOCATION_FAILED(-6) is wired to this value");
    static_assert(static_cast<int>(WMA_ERROR_NOT_INITIALIZED) == -1,
                  "NativeErrorCode.ENGINE_NOT_INITIALIZED(-1) is wired to this value");

    EXPECT_EQ(wma_looper_prepare_track(mWma, /*track=*/-1, 8000, kSampleRate),
              WMA_ERROR_MEMORY);
}

TEST_F(CApiLooperTest, ClearingATrackDeactivatesIt) {
    startAt(kSampleRate, 0);
    recordTrack(0);
    ASSERT_TRUE(wma_looper_is_track_active(mWma, 0));

    wma_looper_clear_track(mWma, 0);
    EXPECT_FALSE(wma_looper_is_track_active(mWma, 0));
}

TEST_F(CApiLooperTest, TheWaveformHonoursTheBufferItWasGiven) {
    startAt(kSampleRate, 0);

    // A bare pointer plus a count: the C API cannot know how big the buffer is,
    // so the count is the contract. Nothing past it may be touched.
    constexpr int kBins = 8;
    std::vector<float> bins(kBins + 4, -12345.0f);
    const int written = wma_looper_get_track_waveform(mWma, 0, bins.data(), kBins);

    EXPECT_LE(written, kBins);
    for (int i = kBins; i < kBins + 4; ++i) {
        EXPECT_FLOAT_EQ(bins[static_cast<size_t>(i)], -12345.0f)
            << "wrote past max_bins at index " << i;
    }
}

// ===========================================================================
// Null handle — every value the JNI used to return by hand
// ===========================================================================
//
// This is the load-bearing test of the batch. Thirty-eight of these defaults
// were written out longhand in jni_audio_bridge.cpp and now live in the C API.
// Each line below is one of them, at the value the JNI returned.

TEST(CApiLooperNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    EXPECT_EQ(wma_looper_prepare_track(nullptr, 0, 8000, 48000), WMA_ERROR_NOT_INITIALIZED);

    EXPECT_FLOAT_EQ(wma_looper_get_record_progress(nullptr), 0.0f);
    EXPECT_FLOAT_EQ(wma_looper_get_progress(nullptr), 0.0f);
    EXPECT_FLOAT_EQ(wma_looper_get_track_progress(nullptr, 0), 0.0f);
    EXPECT_FLOAT_EQ(wma_looper_get_track_peak(nullptr, 0), 0.0f);

    // 1.0f, not 0.0f: silence is not the right answer for a volume or a speed,
    // and the JNI was careful about it. Worth pinning precisely because a
    // zero-initialised default would look plausible.
    EXPECT_FLOAT_EQ(wma_looper_get_master_volume(nullptr), 1.0f);
    EXPECT_FLOAT_EQ(wma_looper_get_track_speed(nullptr, 0), 1.0f);

    EXPECT_EQ(wma_looper_get_master_loop_frames(nullptr), 0);
    EXPECT_EQ(wma_looper_get_track_length_frames(nullptr, 0), 0);
    EXPECT_EQ(wma_looper_get_track_loop_start(nullptr, 0), 0);
    EXPECT_EQ(wma_looper_get_track_loop_end(nullptr, 0), 0);

    EXPECT_FALSE(wma_looper_is_playing(nullptr));
    EXPECT_FALSE(wma_looper_is_recording(nullptr));
    EXPECT_FALSE(wma_looper_is_track_active(nullptr, 0));
    EXPECT_FALSE(wma_looper_is_track_playing(nullptr, 0));
    EXPECT_FALSE(wma_looper_has_undo(nullptr, 0));
    EXPECT_FALSE(wma_looper_save_undo(nullptr, 0));
    EXPECT_FALSE(wma_looper_restore_undo(nullptr, 0));

    EXPECT_FALSE(wma_looper_export_mix(nullptr, "/tmp/nope.wav"));
    EXPECT_FALSE(wma_looper_export_track(nullptr, 0, "/tmp/nope.wav"));
    EXPECT_FALSE(wma_looper_import_track(nullptr, 0, "/tmp/nope.wav", 48000));

    float bins[4];
    EXPECT_EQ(wma_looper_get_track_waveform(nullptr, 0, bins, 4), 0);
}

TEST(CApiLooperNullHandle, ANullPathIsRefusedRatherThanDereferenced) {
    // The JNI could not hit this — GetStringUTFChars on a non-null jstring always
    // gives a pointer — but iOS calls these directly, so the guard is real.
    EXPECT_FALSE(wma_looper_export_mix(nullptr, nullptr));
    EXPECT_FALSE(wma_looper_export_track(nullptr, 0, nullptr));
    EXPECT_FALSE(wma_looper_import_track(nullptr, 0, nullptr, 48000));
}

TEST(CApiLooperNullHandle, EveryMutatorIsANoOpRatherThanACrash) {
    wma_looper_start_recording(nullptr, 0);
    wma_looper_stop_recording(nullptr);
    wma_looper_start_overdub(nullptr, 0);
    wma_looper_stop_all(nullptr);
    wma_looper_pause(nullptr);
    wma_looper_resume(nullptr);
    wma_looper_pause_track(nullptr, 0);
    wma_looper_resume_track(nullptr, 0);
    wma_looper_clear_track(nullptr, 0);
    wma_looper_clear_all(nullptr);
    wma_looper_set_enabled(nullptr, true);
    wma_looper_set_free_length(nullptr, true);
    wma_looper_set_master_volume(nullptr, 0.5f);
    wma_looper_set_track_muted(nullptr, 0, true);
    wma_looper_set_track_pan(nullptr, 0, 0.5f);
    wma_looper_set_track_volume(nullptr, 0, 0.5f);
    wma_looper_set_track_speed(nullptr, 0, 0.5f);
    wma_looper_set_track_loop_region(nullptr, 0, 0, 100);
    wma_looper_reset_track_loop_region(nullptr, 0);
    wma_looper_reset_track_playhead(nullptr, 0);
    wma_looper_trigger_click(nullptr, true);
    SUCCEED();
}

}  // namespace
}  // namespace wma_test
