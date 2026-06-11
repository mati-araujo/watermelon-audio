// Validates Transport state: BPM/beats-per-bar clamping, frames-per-beat math,
// metronome scheduling arming, play-frame counter accounting, and the
// nextBarBoundary quantizer used by armed recording.
//
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include "Transport.h"
#include "AudioLooper.h"

TEST(Transport, BpmClampedToRange) {
    Transport t;
    t.setBpm(5.0f);
    EXPECT_FLOAT_EQ(t.getBpm(), Transport::MIN_BPM);
    t.setBpm(1000.0f);
    EXPECT_FLOAT_EQ(t.getBpm(), Transport::MAX_BPM);
    t.setBpm(140.0f);
    EXPECT_FLOAT_EQ(t.getBpm(), 140.0f);
}

TEST(Transport, BeatsPerBarClampedToRange) {
    Transport t;
    t.setBeatsPerBar(0);
    EXPECT_EQ(t.getBeatsPerBar(), Transport::MIN_BEATS_PER_BAR);
    t.setBeatsPerBar(99);
    EXPECT_EQ(t.getBeatsPerBar(), Transport::MAX_BEATS_PER_BAR);
    t.setBeatsPerBar(3);
    EXPECT_EQ(t.getBeatsPerBar(), 3);
}

TEST(Transport, FramesPerBeatMathAtCommonRates) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    // 60/120 * 48000 = 24000 frames per beat
    EXPECT_EQ(t.framesPerBeat(), 24000);

    t.setBpm(60.0f);
    EXPECT_EQ(t.framesPerBeat(), 48000);

    t.setSampleRate(44100);
    t.setBpm(120.0f);
    // 60/120 * 44100 = 22050
    EXPECT_EQ(t.framesPerBeat(), 22050);
}

TEST(Transport, FramesPerBarHonoursBeatsPerBar) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    t.setBeatsPerBar(4);
    EXPECT_EQ(t.framesPerBar(1), 24000 * 4);
    EXPECT_EQ(t.framesPerBar(2), 24000 * 4 * 2);
    EXPECT_EQ(t.framesPerBar(0), 0);

    t.setBeatsPerBar(3);
    EXPECT_EQ(t.framesPerBar(1), 24000 * 3);
}

TEST(Transport, MetronomeArmsAndStops) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);

    EXPECT_FALSE(t.isMetronomeRunning());
    t.startMetronome(4);
    EXPECT_TRUE(t.isMetronomeRunning());
    EXPECT_EQ(t.getRemainingBeats(), 4);

    t.stopMetronome();
    EXPECT_FALSE(t.isMetronomeRunning());
    EXPECT_EQ(t.getRemainingBeats(), 0);
}

TEST(Transport, MetronomeIgnoresNonPositiveBeats) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    t.startMetronome(0);
    EXPECT_FALSE(t.isMetronomeRunning());
    t.startMetronome(-3);
    EXPECT_FALSE(t.isMetronomeRunning());
}

TEST(Transport, PlayFrameAdvancesOnTick) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    AudioLooper looper;  // tick requires a real looper for triggerClick

    EXPECT_EQ(t.getPlayFrame(), 0);
    t.tick(480, looper);
    EXPECT_EQ(t.getPlayFrame(), 480);
    t.tick(192, looper);
    EXPECT_EQ(t.getPlayFrame(), 672);

    t.resetPlayPosition();
    EXPECT_EQ(t.getPlayFrame(), 0);
}

TEST(Transport, ScheduledMetronomeRendersClickWhenLooperDisabled) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);

    AudioLooper looper;
    looper.setSampleRate(48000);
    looper.setEnabled(false);

    std::array<float, 256> buffer{};
    t.startMetronome(4);
    t.tick(128, looper);
    looper.process(buffer.data(), 128);

    EXPECT_TRUE(std::any_of(buffer.begin(), buffer.end(), [](float sample) {
        return sample != 0.0f;
    }));
    EXPECT_EQ(t.getRemainingBeats(), 3);
}

TEST(AudioLooper, DirectClickRendersWhenDisabled) {
    AudioLooper looper;
    looper.setSampleRate(48000);
    looper.setEnabled(false);

    std::array<float, 256> buffer{};
    looper.triggerClick(true);
    looper.process(buffer.data(), 128);

    EXPECT_TRUE(std::any_of(buffer.begin(), buffer.end(), [](float sample) {
        return sample != 0.0f;
    }));
}

TEST(AudioLooper, ClickIsRenderedAfterRecordingTapWhenEnabled) {
    AudioLooper looper;
    looper.setSampleRate(48000);
    ASSERT_TRUE(looper.prepareTrack(0, 512, 48000));
    looper.startRecording(0);

    std::array<float, 256> buffer{};
    looper.triggerClick(true);
    looper.process(buffer.data(), 128);

    EXPECT_TRUE(std::any_of(buffer.begin(), buffer.end(), [](float sample) {
        return sample != 0.0f;
    }));

    const float* recorded = looper.getTrack(0).data();
    EXPECT_TRUE(std::all_of(recorded, recorded + buffer.size(), [](float sample) {
        return sample == 0.0f;
    }));
}

TEST(Transport, NextBarBoundaryQuantizes) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    t.setBeatsPerBar(4);
    const int64_t fpb = t.framesPerBar(1);  // 96000

    // Already on a boundary → returns same frame.
    EXPECT_EQ(t.nextBarBoundary(0), 0);
    EXPECT_EQ(t.nextBarBoundary(fpb), fpb);
    EXPECT_EQ(t.nextBarBoundary(fpb * 2), fpb * 2);

    // Inside a bar → next multiple.
    EXPECT_EQ(t.nextBarBoundary(1), fpb);
    EXPECT_EQ(t.nextBarBoundary(fpb - 1), fpb);
    EXPECT_EQ(t.nextBarBoundary(fpb + 1), fpb * 2);
}
