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
    EXPECT_NE(tb.data()[0], 0.4f);

    EXPECT_TRUE(tb.restoreUndo());
    EXPECT_FALSE(tb.hasUndo());
    // After restore, original constant content is back.
    EXPECT_FLOAT_EQ(tb.data()[0], 0.4f);
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
    EXPECT_FLOAT_EQ(tb.data()[15000 * 2], 0.0f);  // padded tail is silent
    EXPECT_FLOAT_EQ(tb.data()[5000 * 2], 0.5f);   // original content preserved
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
    EXPECT_GT(std::fabs(tb.data()[0]), 0.0f);
    EXPECT_FLOAT_EQ(tb.data()[5000 * 2], 0.0f);  // past the tail window → untouched
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
