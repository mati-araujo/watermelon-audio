// Validates TrackBuffer: allocation with tail, write head capping at the
// loop boundary, hasReachedLoopEnd, undo save/restore, and basic playback
// via mixInto.
#include <gtest/gtest.h>
#include "TrackBuffer.h"

#include <cmath>
#include <vector>

namespace {

// Write `numFrames` of a constant value (left=value, right=-value).
void writeConstant(TrackBuffer& tb, float value, int numFrames) {
    for (int i = 0; i < numFrames; ++i) {
        tb.writeFrame(value, -value);
    }
}

}  // namespace

TEST(TrackBuffer, AllocateSucceedsAndReportsSizes) {
    TrackBuffer tb;
    const int loop = 24000;
    const int tail = 12000;
    EXPECT_GT(tb.allocate(loop, 48000, tail), 0u);
    EXPECT_EQ(tb.getLoopCapacityFrames(), loop);
    EXPECT_EQ(tb.getTailFrames(), tail);
    // Buffer is now exactly the loop body — the wrap-mix tail overdubs into the
    // start at record time, so no separate tail region is allocated.
    EXPECT_EQ(tb.getCapacityFrames(), loop);
}

TEST(TrackBuffer, AllocateRejectsNonPositiveLoop) {
    TrackBuffer tb;
    EXPECT_EQ(tb.allocate(0, 48000), 0u);
    EXPECT_EQ(tb.allocate(-1, 48000), 0u);
}

TEST(TrackBuffer, TailCappedToLoopFrames) {
    TrackBuffer tb;
    // Pathological case: tail bigger than loop should be capped.
    tb.allocate(1000, 48000, 5000);
    EXPECT_EQ(tb.getTailFrames(), 1000);
}

TEST(TrackBuffer, WriteFrameSaturatesLengthAtLoopCap) {
    TrackBuffer tb;
    const int loop = 256;
    const int tail = 64;
    tb.allocate(loop, 48000, tail);

    writeConstant(tb, 0.5f, loop);
    EXPECT_EQ(tb.getLengthFrames(), loop);
    EXPECT_TRUE(tb.hasReachedLoopEnd());

    // Writing into the tail must NOT increase the playable loop length.
    writeConstant(tb, 0.5f, tail);
    EXPECT_EQ(tb.getLengthFrames(), loop);

    // Past total capacity, writes return false.
    EXPECT_FALSE(tb.writeFrame(0.5f, -0.5f));
}

TEST(TrackBuffer, HasReachedLoopEndFalseBeforeBoundary) {
    TrackBuffer tb;
    tb.allocate(1000, 48000, 0);
    writeConstant(tb, 0.1f, 500);
    EXPECT_FALSE(tb.hasReachedLoopEnd());
    writeConstant(tb, 0.1f, 500);
    EXPECT_TRUE(tb.hasReachedLoopEnd());
}

TEST(TrackBuffer, FinalizeRecordingActivatesTrack) {
    TrackBuffer tb;
    tb.allocate(512, 48000);
    writeConstant(tb, 0.25f, 512);
    EXPECT_FALSE(tb.isActive());
    tb.finalizeRecording();
    EXPECT_TRUE(tb.isActive());
}

TEST(TrackBuffer, MixIntoProducesAudioWhenActiveAndPlaying) {
    TrackBuffer tb;
    const int loop = 1024;
    tb.allocate(loop, 48000);
    writeConstant(tb, 0.5f, loop);
    tb.finalizeRecording();
    tb.setPlaying(true);
    tb.setVolume(1.0f);
    tb.setPan(0.0f);
    tb.setMuted(false);

    std::vector<float> out(256 * 2, 0.0f);
    tb.mixInto(out.data(), 256);

    // After mixInto, output should contain non-zero values (constant signal × pan × vol).
    float maxAbs = 0.0f;
    for (float s : out) maxAbs = std::max(maxAbs, std::fabs(s));
    EXPECT_GT(maxAbs, 0.0f);
}

TEST(TrackBuffer, MixIntoSilentWhenNotPlaying) {
    TrackBuffer tb;
    tb.allocate(1024, 48000);
    writeConstant(tb, 0.5f, 1024);
    tb.finalizeRecording();
    tb.setPlaying(false);

    std::vector<float> out(256 * 2, 0.0f);
    tb.mixInto(out.data(), 256);
    for (float s : out) EXPECT_FLOAT_EQ(s, 0.0f);
}

TEST(TrackBuffer, MixIntoSilentWhenInactive) {
    TrackBuffer tb;
    tb.allocate(1024, 48000);
    writeConstant(tb, 0.5f, 1024);
    // Note: no finalizeRecording → not active.
    tb.setPlaying(true);

    std::vector<float> out(256 * 2, 0.0f);
    tb.mixInto(out.data(), 256);
    for (float s : out) EXPECT_FLOAT_EQ(s, 0.0f);
}

TEST(TrackBuffer, UndoSaveAndRestoreRoundTrip) {
    TrackBuffer tb;
    tb.allocate(512, 48000);
    writeConstant(tb, 0.4f, 512);
    tb.finalizeRecording();

    EXPECT_FALSE(tb.hasUndo());
    EXPECT_TRUE(tb.saveUndoSnapshot());
    EXPECT_TRUE(tb.hasUndo());

    // Overdub a different value into the first sample, then restore.
    tb.overdubFrame(0, /*left=*/1.0f, /*right=*/1.0f, /*gain=*/1.0f, /*decay=*/1.0f);
    EXPECT_NE(tb.sampleAt(0, 0), 0.4f);

    EXPECT_TRUE(tb.restoreUndo());
    EXPECT_FALSE(tb.hasUndo());
    // After restore, original constant content is back.
    EXPECT_FLOAT_EQ(tb.sampleAt(0, 0), 0.4f);
}

TEST(TrackBuffer, RestoreUndoFailsWithoutSnapshot) {
    TrackBuffer tb;
    tb.allocate(256, 48000);
    EXPECT_FALSE(tb.restoreUndo());
}

TEST(TrackBuffer, ClearResetsLengthAndState) {
    TrackBuffer tb;
    tb.allocate(1024, 48000, 128);
    writeConstant(tb, 0.5f, 1024);
    tb.finalizeRecording();
    tb.setPlaying(true);

    tb.clear();
    EXPECT_FALSE(tb.isActive());
    EXPECT_FALSE(tb.isTrackPlaying());
    EXPECT_EQ(tb.getLengthFrames(), 0);
    EXPECT_EQ(tb.getCapacityFrames(), 0);
    EXPECT_EQ(tb.getLoopCapacityFrames(), 0);
    EXPECT_EQ(tb.getTailFrames(), 0);
}

// ----- Free-loop auto-sync: onset detection (phase B) -----

TEST(TrackBuffer, DetectOnsetsFindsEvenlySpacedTransients) {
    TrackBuffer tb;
    const int period = 12000;            // 250ms @48k
    const int count = 6;
    const int total = period * (count + 1);
    tb.allocate(total, 48000, 0);
    // Lead-in silence, then `count` loud bursts at period boundaries.
    for (int i = 0; i < period; ++i) tb.writeFrame(0.0005f, 0.0005f);
    for (int k = 0; k < count; ++k) {
        for (int i = 0; i < period; ++i) {
            const float v = (i < 256) ? 0.6f : 0.0005f;
            tb.writeFrame(v, v);
        }
    }

    int onsets[64];
    const int n = tb.detectOnsets(onsets, 64, 256, 1.0f);
    EXPECT_GE(n, count - 1);
    EXPECT_LE(n, count + 1);
    for (int i = 1; i < n; ++i) {
        EXPECT_NEAR(onsets[i] - onsets[i - 1], period, 256 * 3);
    }
}

TEST(TrackBuffer, DetectOnsetsEmptyOnSilence) {
    TrackBuffer tb;
    tb.allocate(8000, 48000, 0);
    writeConstant(tb, 0.0f, 8000);
    int onsets[16];
    EXPECT_EQ(tb.detectOnsets(onsets, 16, 256, 1.0f), 0);
}

// A sustained low tone (period > hop) must NOT flood the detector with false
// onsets — only its single attack counts. Regression guard for the held-synth
// over-triggering (a real take reported >100 onsets / 6 s before the energy
// smoothing + flux floor).
TEST(TrackBuffer, DetectOnsetsDoesNotOverTriggerOnSustainedTone) {
    TrackBuffer tb;
    const int frames = 120000;  // 2.5 s
    tb.allocate(frames, 48000, 0);
    for (int i = 0; i < frames; ++i) {
        // Tiny lead-in silence so the attack is a real rising edge, then a steady
        // 80 Hz tone (period 600 frames ≫ 256 hop → raw windowed energy wobbles).
        const float s = (i < 600) ? 0.0f
            : 0.4f * std::sin(2.0f * static_cast<float>(M_PI) * 80.0f * i / 48000.0f);
        tb.writeFrame(s, s);
    }
    int onsets[128];
    const int n = tb.detectOnsets(onsets, 128, 256, 1.0f);
    EXPECT_LE(n, 3);  // ~the attack only, not dozens of wobble-driven false hits
}

// ----- Free-loop auto-sync: bar-snap + seam bake (phases A + C) -----

TEST(TrackBuffer, FinalizeFreeLoopSetsRegionAndRestarts) {
    TrackBuffer tb;
    const int len = 20000;
    tb.allocate(len, 48000, 0);
    writeConstant(tb, 0.5f, len);
    tb.finalizeRecording();
    tb.setPlaying(true);

    EXPECT_TRUE(tb.finalizeFreeLoop(1000, 9000, /*tailFrames=*/0));
    EXPECT_EQ(tb.getLoopStart(), 1000);
    EXPECT_EQ(tb.getLoopEnd(), 9000);
    EXPECT_EQ(tb.getPlayHead(), 0);
    EXPECT_TRUE(tb.isTrackPlaying());  // resumes — it was playing
}

TEST(TrackBuffer, FinalizeFreeLoopPadsWithSilencePastRecording) {
    TrackBuffer tb;
    const int len = 10000;
    tb.allocate(len, 48000, 0);
    writeConstant(tb, 0.5f, len);
    tb.finalizeRecording();

    // loopEnd beyond the recording → grow with trailing silence to close on grid.
    EXPECT_TRUE(tb.finalizeFreeLoop(0, 16000, /*tailFrames=*/0));
    EXPECT_EQ(tb.getLengthFrames(), 16000);
    EXPECT_EQ(tb.getLoopEnd(), 16000);
    EXPECT_FLOAT_EQ(tb.sampleAt(15000, 0), 0.0f);  // padded tail is silent
    EXPECT_FLOAT_EQ(tb.sampleAt(5000, 0), 0.5f);   // original content preserved
}

TEST(TrackBuffer, FinalizeFreeLoopBakesSeamFromContinuation) {
    TrackBuffer tb;
    const int len = 20000;
    tb.allocate(len, 48000, 0);
    // Silent loop body, loud continuation past the loop end.
    for (int i = 0; i < len; ++i) {
        const float v = (i >= 10000) ? 0.5f : 0.0f;
        tb.writeFrame(v, v);
    }
    tb.finalizeRecording();

    EXPECT_TRUE(tb.finalizeFreeLoop(0, 10000, /*tailFrames=*/2000));
    // The continuation has been baked into the loop start with a decaying fade.
    EXPECT_GT(std::fabs(tb.sampleAt(0, 0)), 0.0f);
    EXPECT_FLOAT_EQ(tb.sampleAt(5000, 0), 0.0f);  // past the tail window → untouched
}

TEST(TrackBuffer, VolumeAndPanClampedToValidRange) {
    TrackBuffer tb;
    tb.allocate(256, 48000);
    tb.setVolume(-1.0f);
    EXPECT_FLOAT_EQ(tb.getVolume(), 0.0f);
    tb.setVolume(99.0f);
    EXPECT_FLOAT_EQ(tb.getVolume(), 2.0f);

    tb.setPan(-5.0f);
    EXPECT_FLOAT_EQ(tb.getPan(), -1.0f);
    tb.setPan(5.0f);
    EXPECT_FLOAT_EQ(tb.getPan(), 1.0f);
}

// ----- QW-1 / QW-2: unity-speed fast path + block gain ramp -----

// At speed 1.0 with an integer playhead and settled gains, the fast path must
// reproduce the recorded content scaled ONLY by the centre-pan gain (-3 dB).
// Uses a per-frame-varying pattern so a copy bug (wrong offset/stride) can't
// hide behind a constant signal.
TEST(TrackBuffer, UnitySpeedFastPathReproducesContent) {
    TrackBuffer tb;
    const int loop = 2048;
    tb.allocate(loop, 48000);
    for (int i = 0; i < loop; ++i) {
        const float l = std::sin(2.0f * static_cast<float>(M_PI) * 3.0f * i / loop);
        const float r = std::cos(2.0f * static_cast<float>(M_PI) * 5.0f * i / loop);
        tb.writeFrame(l, r);
    }
    tb.finalizeRecording();
    tb.setPlaying(true);
    tb.setVolume(1.0f);
    tb.setPan(0.0f);   // centre → equal-power -3 dB ≈ 0.7071 on both channels
    tb.setMuted(false);

    const int block = 512;
    std::vector<float> out(block * 2, 0.0f);
    tb.mixInto(out.data(), block);  // playhead 0 → fast path (no wrap, no seam)

    // Expected per-channel gain comes from the SAME quantized equal-power LUT the
    // mixer uses (centre index ≈ -3 dB but not perfectly symmetric); hardcoding
    // 0.7071 would false-fail on the LUT quantization.
    const auto centre = wm::EqualPowerPanLUT::instance().lookup(0.0f);
    for (int i = 0; i < block; ++i) {
        const float l = std::sin(2.0f * static_cast<float>(M_PI) * 3.0f * i / loop);
        const float r = std::cos(2.0f * static_cast<float>(M_PI) * 5.0f * i / loop);
        EXPECT_NEAR(out[i * 2],     l * centre.l, 1e-4f) << "L frame " << i;
        EXPECT_NEAR(out[i * 2 + 1], r * centre.r, 1e-4f) << "R frame " << i;
    }
}

// Multi-page loop: the unity-speed fast path must read correctly ACROSS a
// chunk-page boundary (32768 frames). In dense mode this is one contiguous run;
// in chunked mode the block splits into per-page runs — this is the regression
// guard for that split (the small-loop tests above stay within one page).
TEST(TrackBuffer, UnitySpeedFastPathCrossesPageBoundary) {
    TrackBuffer tb;
    const int loop = 40000;                   // > 32768 → spans 2 chunk pages
    tb.allocate(loop, 48000);
    auto content = [](int i) { return static_cast<float>((i % 997)) / 997.0f - 0.5f; };
    for (int i = 0; i < loop; ++i) tb.writeFrame(content(i), content(i));
    tb.finalizeRecording();
    tb.setPlaying(true);
    tb.setVolume(1.0f); tb.setPan(0.0f); tb.setMuted(false);

    const auto centre = wm::EqualPowerPanLUT::instance().lookup(0.0f);
    const int block = 3000;                   // unaligned to the page → blocks straddle 32768
    std::vector<float> out(block * 2);
    int pos = 0;
    // Stay clear of the seam crossfade window (last ~2400 frames) so we remain on
    // the fast path; cover [0, 36000) which includes the page boundary at 32768.
    for (int blk = 0; blk < 12; ++blk) {
        std::fill(out.begin(), out.end(), 0.0f);
        tb.mixInto(out.data(), block);
        for (int i = 0; i < block; ++i) {
            const float expected = content(pos + i);
            EXPECT_NEAR(out[i * 2],     expected * centre.l, 1e-4f) << "L pos " << (pos + i);
            EXPECT_NEAR(out[i * 2 + 1], expected * centre.r, 1e-4f) << "R pos " << (pos + i);
        }
        pos += block;
    }
}

// Non-unity speed must stay on the interpolating slow path and still produce
// bounded audio (no NaN/Inf from the Catmull-Rom + fmod path).
TEST(TrackBuffer, NonUnitySpeedProducesBoundedAudio) {
    TrackBuffer tb;
    const int loop = 2048;
    tb.allocate(loop, 48000);
    for (int i = 0; i < loop; ++i) {
        const float v = 0.5f * std::sin(2.0f * static_cast<float>(M_PI) * 4.0f * i / loop);
        tb.writeFrame(v, v);
    }
    tb.finalizeRecording();
    tb.setPlaying(true);
    tb.setSpeed(0.5f);

    std::vector<float> out(512 * 2, 0.0f);
    float maxAbs = 0.0f;
    for (int blk = 0; blk < 8; ++blk) {          // sweep across the loop + seam
        std::fill(out.begin(), out.end(), 0.0f);
        tb.mixInto(out.data(), 512);
        for (float s : out) {
            EXPECT_TRUE(std::isfinite(s));
            maxAbs = std::max(maxAbs, std::fabs(s));
        }
    }
    EXPECT_GT(maxAbs, 0.0f);
    EXPECT_LT(maxAbs, 2.0f);
}

// ----- QW-6: live incremental waveform -----

// Mid-recording, getLiveWaveform must reflect the captured prefix (non-zero)
// and leave the not-yet-recorded tail at zero — a left-to-right fill — without
// any O(n) buffer scan.
TEST(TrackBuffer, LiveWaveformReflectsRecordedPrefix) {
    TrackBuffer tb;
    const int loop = 4096;               // framesPerBin = 4096/512 = 8
    tb.allocate(loop, 48000);
    writeConstant(tb, 0.5f, loop / 2);   // record exactly the first half

    const int numBins = 64;
    std::vector<float> bins(numBins, -1.0f);
    EXPECT_EQ(tb.getLiveWaveform(bins.data(), numBins), numBins);

    // First half of the bins map to recorded frames → ~0.5 peak.
    for (int i = 0; i < numBins / 2 - 1; ++i) {
        EXPECT_GT(bins[i], 0.4f) << "recorded bin " << i;
    }
    // Second half maps past the write head → still silent.
    for (int i = numBins / 2 + 1; i < numBins; ++i) {
        EXPECT_FLOAT_EQ(bins[i], 0.0f) << "unrecorded bin " << i;
    }
}

// A freshly allocated (unrecorded) track yields an all-zero live waveform.
TEST(TrackBuffer, LiveWaveformZeroBeforeRecording) {
    TrackBuffer tb;
    tb.allocate(2048, 48000);
    std::vector<float> bins(32, -1.0f);
    EXPECT_EQ(tb.getLiveWaveform(bins.data(), 32), 32);
    for (float b : bins) EXPECT_FLOAT_EQ(b, 0.0f);
}

// clear() must wipe the live waveform so a reused track doesn't show ghosts.
TEST(TrackBuffer, LiveWaveformClearedOnClear) {
    TrackBuffer tb;
    tb.allocate(2048, 48000);
    writeConstant(tb, 0.5f, 2048);
    tb.clear();
    tb.allocate(2048, 48000);
    std::vector<float> bins(32, -1.0f);
    tb.getLiveWaveform(bins.data(), 32);
    for (float b : bins) EXPECT_FLOAT_EQ(b, 0.0f);
}

// ----- F3.4: finite play count -----

// A track set to play N times auto-stops after N passes of its loop region and
// latches "completed" exactly once. Infinite (default) never stops.
TEST(TrackBuffer, PlayCountStopsAfterNPasses) {
    TrackBuffer tb;
    const int loop = 2048;
    tb.allocate(loop, 48000);
    writeConstant(tb, 0.5f, loop);
    tb.finalizeRecording();
    tb.setPlaying(true);
    tb.setPlayCount(2);                       // two passes then stop

    std::vector<float> out(512 * 2, 0.0f);
    bool stopped = false;
    int blocksToStop = 0;
    for (int blk = 0; blk < 12 && !stopped; ++blk) {
        std::fill(out.begin(), out.end(), 0.0f);
        tb.mixInto(out.data(), 512);
        ++blocksToStop;
        stopped = !tb.isTrackPlaying();
    }
    EXPECT_TRUE(stopped);
    EXPECT_EQ(tb.getRemainingPlays(), 0);
    // 2 passes × 2048 = 4096 frames = 8 blocks of 512.
    EXPECT_EQ(blocksToStop, 8);
    EXPECT_TRUE(tb.consumeCompleted());       // latched once
    EXPECT_FALSE(tb.consumeCompleted());      // and only once
}

TEST(TrackBuffer, PlayCountInfiniteByDefaultNeverStops) {
    TrackBuffer tb;
    const int loop = 1024;
    tb.allocate(loop, 48000);
    writeConstant(tb, 0.5f, loop);
    tb.finalizeRecording();
    tb.setPlaying(true);
    // No setPlayCount → infinite.

    std::vector<float> out(512 * 2, 0.0f);
    for (int blk = 0; blk < 40; ++blk) {      // 20+ passes
        std::fill(out.begin(), out.end(), 0.0f);
        tb.mixInto(out.data(), 512);
    }
    EXPECT_TRUE(tb.isTrackPlaying());
    EXPECT_EQ(tb.getRemainingPlays(), -1);
    EXPECT_FALSE(tb.consumeCompleted());
}

// Mute must ramp the block gain down to (near) silence within a few blocks,
// confirming the block-ramped mute smoother still converges.
TEST(TrackBuffer, MuteRampsToSilence) {
    TrackBuffer tb;
    const int loop = 2048;
    tb.allocate(loop, 48000);
    writeConstant(tb, 0.5f, loop);
    tb.finalizeRecording();
    tb.setPlaying(true);
    tb.setMuted(true);

    std::vector<float> out(512 * 2, 0.0f);
    for (int blk = 0; blk < 12; ++blk) {   // ~128ms of settling at 48k
        std::fill(out.begin(), out.end(), 0.0f);
        tb.mixInto(out.data(), 512);
    }
    float maxAbs = 0.0f;
    for (float s : out) maxAbs = std::max(maxAbs, std::fabs(s));
    EXPECT_LT(maxAbs, 1e-3f);
}

// A large-capacity take that records only a little and is trimmed hands the unused
// backing store back: reservedBytes() collapses toward the recorded content. Dense
// reallocs its vector; chunked returns pool chunks to the OS — same observable
// outcome, and the reason the paged backend can default on without a RAM
// regression (plan §3.1). Storage-agnostic.
TEST(TrackBuffer, TrimReclaimsReservedMemory) {
    TrackBuffer tb;
    const int cap = 2'000'000;                     // ~16 MB backing store
    ASSERT_GT(tb.allocate(cap, 48000), 0u);
    const size_t reservedFull = tb.reservedBytes();
    EXPECT_GT(reservedFull, 8u * 1024 * 1024) << "prepare reserves the full capacity";

    writeConstant(tb, 0.5f, 50'000);               // record ~1 s of the 40 s capacity
    tb.finalizeRecording();
    EXPECT_EQ(tb.getLengthFrames(), 50'000);

    ASSERT_TRUE(tb.trimToLength());
    const size_t reservedTrimmed = tb.reservedBytes();
    EXPECT_LT(reservedTrimmed, reservedFull / 4) << "trim must reclaim the unused RAM";
}
