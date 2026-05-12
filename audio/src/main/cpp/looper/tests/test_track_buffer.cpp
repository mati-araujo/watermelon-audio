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
    EXPECT_EQ(tb.getCapacityFrames(), loop + tail);
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
