// ============================================================================
// test_audio_looper — safety net for AudioLooper before the F2 refactor.
//
// Covers the areas the plan (docs/looper_pro_memory_plan.md §4) flags as having
// no coverage today: memory budget accounting, the recording FSM
// (armed→trigger→record→loop-boundary→tail→finalize, abort, free-at-cap),
// WAV import + resample, and WAV export (snapshot length math: repeat / count-in
// / bit depth, plus an export→import round-trip).
//
// Host-side (x86) — AudioLooper.h is header-only; WavFile does real file IO,
// which works on the host. No Oboe / JNI / device needed.
// ============================================================================
#include <gtest/gtest.h>
#include "AudioLooper.h"

#include "WavFile.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kSR = 48000;

// Drive `process()` with a constant stereo signal, in `block`-sized chunks,
// advancing the transport play frame so armed triggers behave realistically.
// Returns the play frame reached at the end.
int64_t feed(AudioLooper& looper, float value, int totalFrames, int block,
             int64_t playFrameStart = 0) {
    std::vector<float> buf(static_cast<size_t>(block) * 2);
    int64_t playFrame = playFrameStart;
    int remaining = totalFrames;
    while (remaining > 0) {
        const int n = std::min(block, remaining);
        for (int i = 0; i < n; ++i) {
            buf[i * 2] = value;
            buf[i * 2 + 1] = value;
        }
        looper.process(buf.data(), n, playFrame);
        playFrame += n;
        remaining -= n;
    }
    return playFrame;
}

// Unique temp path for a WAV artifact; removed by WavTempFile's dtor.
struct WavTempFile {
    std::filesystem::path path;
    explicit WavTempFile(const std::string& name)
        : path(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    ~WavTempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

// Write a stereo WAV of `frames` at `sr`, both channels = `value`.
bool makeWav(const std::string& path, int frames, int sr, float value,
             wav::BitDepth depth = wav::BitDepth::PCM_16) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2, value);
    return wav::writeWav(path.c_str(), buf.data(), frames, sr, depth);
}

}  // namespace

// ======================= Memory budget =======================

// These two assert the DENSE budget model, where prepareTrack reserves the full
// capacity up front. The paged (chunked) model deliberately diverges — an empty
// prepared track costs 0 budget until audio is written (silence = no memory), so
// the chunked equivalents live below under WM_LOOPER_CHUNKED_BUFFER.
#ifndef WM_LOOPER_CHUNKED_BUFFER

// Prepare tracks until the 48 MB budget is exhausted → clean failure, no crash.
// Clearing a track frees its bytes so a subsequent prepare succeeds (regression
// for "2nd free take fails" — clear() must return capacity to the budget).
TEST(AudioLooper, BudgetExhaustsThenRecoversAfterClear) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    // 3M frames = 3M * 2ch * 4B = 24 MB each. Two fit under 48 MB, a third can't.
    const int big = 3'000'000;
    EXPECT_TRUE(looper.prepareTrack(0, big, kSR));
    EXPECT_TRUE(looper.prepareTrack(1, big, kSR));
    EXPECT_FALSE(looper.prepareTrack(2, big, kSR)) << "third 24MB track must exceed 48MB";

    // Free one track → budget recovered → the third prepare now succeeds.
    looper.clearTrack(1);
    EXPECT_TRUE(looper.prepareTrack(2, big, kSR));
}

// Re-preparing an already-allocated track must not double-count its bytes
// (budget uses currentUsage - trackCurrent + needed).
TEST(AudioLooper, ReprepareSameTrackDoesNotDoubleCount) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    // 6M frames = 48 MB — right up against the budget.
    const int nearMax = 6'000'000;
    ASSERT_TRUE(looper.prepareTrack(0, nearMax, kSR));
    // Replacing it with the same size must succeed; double-counting would make
    // this look like 96 MB and fail.
    EXPECT_TRUE(looper.prepareTrack(0, nearMax, kSR));
}

#else  // WM_LOOPER_CHUNKED_BUFFER — paged budget model

// An empty prepared take costs no budget (silent pages aren't materialised), so
// two 6M-frame free takes — 96 MB of dense capacity, well over the 48 MB cap —
// both prepare successfully. This is the "free take no longer pre-consumes the
// budget" win the paging is for.
TEST(AudioLooper, ChunkedEmptyTakesDoNotConsumeBudget) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    EXPECT_TRUE(looper.prepareTrack(0, 6'000'000, kSR));
    EXPECT_TRUE(looper.prepareTrack(1, 6'000'000, kSR));
    // A short take still records fine afterwards.
    looper.setTailMs(0);
    ASSERT_TRUE(looper.prepareTrack(2, 24000, kSR));
    looper.startRecording(2);
    feed(looper, 0.5f, 24000, 512);
    EXPECT_TRUE(looper.isTrackActive(2));
}

#endif  // WM_LOOPER_CHUNKED_BUFFER

// ======================= Runtime capabilities (F3) =======================

TEST(AudioLooper, CapabilitiesDefaults) {
    AudioLooper looper;
    const auto c = looper.getCapabilities();
    EXPECT_EQ(c.memoryBudgetBytes, AudioLooper::DEFAULT_MEMORY_BUDGET_BYTES);
    EXPECT_EQ(c.maxActiveTracks, AudioLooper::DEFAULT_MAX_ACTIVE_TRACKS);  // 8
    EXPECT_EQ(c.maxFreeSeconds, AudioLooper::DEFAULT_MAX_FREE_SECONDS);    // 60
}

// A higher tier unlocks tracks 8..15 up to the hardware ceiling.
TEST(AudioLooper, SetMaxActiveTracksUnlocksMoreTracks) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    EXPECT_FALSE(looper.prepareTrack(8, 24000, kSR)) << "track 8 blocked at default limit 8";

    AudioLooper::LooperCapabilities caps;
    caps.maxActiveTracks = 16;
    looper.setCapabilities(caps);
    EXPECT_EQ(looper.getMaxActiveTracks(), 16);

    EXPECT_TRUE(looper.prepareTrack(8, 24000, kSR));
    EXPECT_TRUE(looper.prepareTrack(15, 24000, kSR));
    EXPECT_FALSE(looper.prepareTrack(16, 24000, kSR)) << "16 is the hardware ceiling";
}

// Lowering the limit must never deactivate an already-active track: the limit is
// clamped up to the highest active index.
TEST(AudioLooper, LoweringMaxActiveTracksKeepsActiveTrack) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.setTailMs(0);

    AudioLooper::LooperCapabilities caps;
    caps.maxActiveTracks = 16;
    looper.setCapabilities(caps);

    ASSERT_TRUE(looper.prepareTrack(10, 24000, kSR));
    looper.startRecording(10);
    feed(looper, 0.5f, 24000, 512);
    ASSERT_TRUE(looper.isTrackActive(10));

    // Ask for 4 — but track 10 is active, so the effective limit clamps to >= 11.
    caps.maxActiveTracks = 4;
    looper.setCapabilities(caps);
    EXPECT_GE(looper.getMaxActiveTracks(), 11);
    EXPECT_TRUE(looper.isTrackActive(10)) << "active track must not be silenced";
    EXPECT_TRUE(looper.isTrackPlaying(10));
}

// Budget is runtime-configurable (dense model: prepare reserves capacity).
#ifndef WM_LOOPER_CHUNKED_BUFFER
TEST(AudioLooper, SetBudgetChangesWhatFits) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    // Shrink the budget to 8 MB → a 24 MB (3M-frame) track no longer fits.
    AudioLooper::LooperCapabilities caps;
    caps.memoryBudgetBytes = 8ULL * 1024 * 1024;
    looper.setCapabilities(caps);
    EXPECT_FALSE(looper.prepareTrack(0, 3'000'000, kSR));

    // Raise it back to 48 MB → it fits again.
    caps.memoryBudgetBytes = 48ULL * 1024 * 1024;
    looper.setCapabilities(caps);
    EXPECT_TRUE(looper.prepareTrack(0, 3'000'000, kSR));
}
#endif

// A finite play count auto-stops the track and pushes a TrackCompleted event
// through the dispatcher end-to-end (emit → queue → worker → sink).
TEST(AudioLooper, PlayCountEmitsTrackCompleted) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.setTailMs(0);

    wm::LooperEventDispatcher dispatcher;
    std::atomic<int> completed{0};
    std::atomic<int> completedTrack{-1};
    dispatcher.setSink([&](const wm::LooperEvent& ev) {
        if (ev.type == wm::LooperEvent::Type::TrackCompleted) {
            completed.fetch_add(1);
            completedTrack.store(ev.trackIndex);
        }
    });
    dispatcher.start();
    looper.setEventDispatcher(&dispatcher);

    const int loop = 2048;
    ASSERT_TRUE(looper.prepareTrack(0, loop, kSR));
    looper.startRecording(0);
    feed(looper, 0.5f, loop, 512);            // finalizes → active + playing at 0
    ASSERT_TRUE(looper.isTrackActive(0));

    looper.setTrackPlayCount(0, 2);           // play twice then stop
    feed(looper, 0.0f, loop * 2 + 1024, 512); // drive past the 2nd wrap

    EXPECT_FALSE(looper.isTrackPlaying(0));
    EXPECT_EQ(looper.getTrackRemainingPlays(0), 0);

    // Let the dispatcher worker drain the queue.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    looper.setEventDispatcher(nullptr);
    dispatcher.stop();

    EXPECT_GE(completed.load(), 1);
    EXPECT_EQ(completedTrack.load(), 0);
}

// ======================= Recording FSM =======================

// No-tail take: after exactly loopFrames captured, the loop finalizes, playback
// starts, and recording clears itself (free-at-cap path — isRecording() must
// not stay stuck true).
TEST(AudioLooper, RecordNoTailFinalizesAndClearsRecordingTrack) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.setTailMs(0);                       // no wrap-mix tail
    const int loop = 24000;                    // 0.5 s
    ASSERT_TRUE(looper.prepareTrack(0, loop, kSR));

    looper.startRecording(0);
    EXPECT_TRUE(looper.isRecording());
    EXPECT_EQ(looper.getRecordingTrack(), 0);

    feed(looper, 0.5f, loop, 512);

    EXPECT_FALSE(looper.isRecording()) << "free-at-cap must clear mRecordingTrack";
    EXPECT_EQ(looper.getRecordingTrack(), -1);
    EXPECT_TRUE(looper.isTrackActive(0));
    EXPECT_TRUE(looper.isTrackPlaying(0));
    EXPECT_EQ(looper.getTrackLengthFrames(0), loop);
}

// With a wrap-mix tail: the loop finalizes (active + playing) at the boundary
// while recording continues to capture the tail; recording clears only after
// loop + tail frames.
TEST(AudioLooper, RecordWithTailFinalizesAtBoundaryClearsAfterTail) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.setTailMs(100);                     // 100ms tail = 4800 frames
    const int loop = 48000;
    ASSERT_TRUE(looper.prepareTrack(0, loop, kSR));
    const int tail = 100 * kSR / 1000;         // 4800

    looper.startRecording(0);
    // Capture exactly the loop body.
    feed(looper, 0.5f, loop, 512);
    EXPECT_TRUE(looper.isTrackActive(0)) << "loop boundary should finalize+activate";
    EXPECT_TRUE(looper.isTrackPlaying(0));
    EXPECT_TRUE(looper.isRecording()) << "tail still capturing";

    // Capture the tail → recording ends.
    feed(looper, 0.5f, tail, 512);
    EXPECT_FALSE(looper.isRecording());
    EXPECT_EQ(looper.getRecordingTrack(), -1);
}

// Armed recording fires exactly when the transport crosses the trigger frame.
TEST(AudioLooper, ArmedRecordingFiresAtTriggerFrame) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.setTailMs(0);
    ASSERT_TRUE(looper.prepareTrack(0, 24000, kSR));

    looper.armRecording(0, /*triggerFrame=*/1000);
    EXPECT_EQ(looper.getArmedTrack(), 0);
    EXPECT_FALSE(looper.isRecording());

    // First block [0,512): does not reach the trigger.
    feed(looper, 0.5f, 512, 512, /*playFrameStart=*/0);
    EXPECT_FALSE(looper.isRecording()) << "trigger not yet crossed";
    EXPECT_EQ(looper.getArmedTrack(), 0);

    // Second block [512,1024): crosses frame 1000 → fires.
    feed(looper, 0.5f, 512, 512, /*playFrameStart=*/512);
    EXPECT_TRUE(looper.isRecording());
    EXPECT_EQ(looper.getArmedTrack(), -1);
    EXPECT_EQ(looper.getArmedTriggered(), 1);
}

// abortRecording throws away a partial take and returns the track to idle.
TEST(AudioLooper, AbortRecordingDiscardsPartialTake) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.setTailMs(0);
    ASSERT_TRUE(looper.prepareTrack(0, 48000, kSR));

    looper.startRecording(0);
    feed(looper, 0.5f, 12000, 512);            // capture only part of the loop
    ASSERT_TRUE(looper.isRecording());

    looper.abortRecording();
    EXPECT_FALSE(looper.isRecording());
    EXPECT_EQ(looper.getRecordingTrack(), -1);
    EXPECT_FALSE(looper.isTrackActive(0)) << "aborted take must be discarded";
    EXPECT_EQ(looper.getTrackLengthFrames(0), 0);
}

// cancelArm clears a pending armed recording without firing it.
TEST(AudioLooper, CancelArmClearsPendingTrigger) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    ASSERT_TRUE(looper.prepareTrack(0, 24000, kSR));

    looper.armRecording(0, 1000);
    ASSERT_EQ(looper.getArmedTrack(), 0);
    looper.cancelArm();
    EXPECT_EQ(looper.getArmedTrack(), -1);

    // Crossing the old trigger must NOT start a recording.
    feed(looper, 0.5f, 2048, 512, 0);
    EXPECT_FALSE(looper.isRecording());
}

// ======================= Import + resample =======================

// A 44.1 kHz source imported at 48 kHz is resampled to the correct length.
TEST(AudioLooper, ImportResamples44kTo48k) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    WavTempFile wf("wm_import_44k.wav");
    ASSERT_TRUE(makeWav(wf.str(), /*frames=*/4410, /*sr=*/44100, 0.5f));

    ASSERT_TRUE(looper.importTrack(0, wf.str().c_str(), kSR));
    // ceil(4410 * 48000/44100) = 4800.
    EXPECT_EQ(looper.getTrackLengthFrames(0), 4800);
    EXPECT_TRUE(looper.isTrackActive(0));
}

// Import must honour the memory budget: with the budget nearly full, importing
// a track that doesn't fit fails cleanly and leaves the target track empty.
// Dense-only: it relies on a prepared-but-empty track reserving its capacity,
// which the paged model intentionally does not do (see budget note above).
#ifndef WM_LOOPER_CHUNKED_BUFFER
TEST(AudioLooper, ImportRespectsBudget) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    // 6M frames = 48 MB used; ~2.3 MB (~291k frames) left under the 48 MB cap.
    ASSERT_TRUE(looper.prepareTrack(1, 6'000'000, kSR));

    WavTempFile wf("wm_import_toobig.wav");
    ASSERT_TRUE(makeWav(wf.str(), /*frames=*/400'000, kSR, 0.5f));  // needs ~3.2 MB

    EXPECT_FALSE(looper.importTrack(0, wf.str().c_str(), kSR));
    EXPECT_FALSE(looper.isTrackActive(0));
    EXPECT_EQ(looper.getTrackLengthFrames(0), 0);
}
#endif  // WM_LOOPER_CHUNKED_BUFFER

// Regression: an imported track must actually play back — importTrack sets
// mEnabled so process() doesn't early-return into silence.
TEST(AudioLooper, ImportedTrackPlaysBack) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    WavTempFile wf("wm_import_play.wav");
    ASSERT_TRUE(makeWav(wf.str(), /*frames=*/4800, kSR, 0.5f));
    ASSERT_TRUE(looper.importTrack(0, wf.str().c_str(), kSR));

    looper.resumeTrack(0);
    std::vector<float> out(512 * 2, 0.0f);
    looper.process(out.data(), 512);

    float maxAbs = 0.0f;
    for (float s : out) maxAbs = std::max(maxAbs, std::fabs(s));
    EXPECT_GT(maxAbs, 0.0f) << "imported track produced silence (mEnabled regression?)";
}

// ======================= Export =======================

// repeatLoops and countInFrames produce exact output lengths, and the count-in
// region is leading silence.
TEST(AudioLooper, ExportRepeatAndCountInLengths) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    WavTempFile src("wm_export_src.wav");
    ASSERT_TRUE(makeWav(src.str(), /*frames=*/4800, kSR, 0.4f));
    ASSERT_TRUE(looper.importTrack(0, src.str().c_str(), kSR));

    WavTempFile out("wm_export_out.wav");
    AudioLooper::ExportOptions opts;
    opts.repeatLoops = 2;
    opts.countInFrames = 500;
    opts.applyLimiter = false;
    ASSERT_TRUE(looper.exportMix(out.str().c_str(), opts));

    wav::WavData wd = wav::readWav(out.str().c_str());
    EXPECT_EQ(wd.numFrames, 4800 * 2 + 500);
    EXPECT_EQ(wd.sampleRate, kSR);
    // Count-in region is silent.
    ASSERT_GE(static_cast<int>(wd.buffer.size()), 500 * 2);
    for (int i = 0; i < 500 * 2; ++i) EXPECT_FLOAT_EQ(wd.buffer[i], 0.0f);
    // Content after the count-in is non-silent.
    float maxAbs = 0.0f;
    for (int i = 500 * 2; i < static_cast<int>(wd.buffer.size()); ++i) {
        maxAbs = std::max(maxAbs, std::fabs(wd.buffer[i]));
    }
    EXPECT_GT(maxAbs, 0.0f);
}

// Export → import round-trip preserves length through the whole WAV pipeline.
TEST(AudioLooper, ExportImportRoundTrip) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    WavTempFile src("wm_rt_src.wav");
    ASSERT_TRUE(makeWav(src.str(), /*frames=*/6000, kSR, 0.4f));
    ASSERT_TRUE(looper.importTrack(0, src.str().c_str(), kSR));

    WavTempFile mix("wm_rt_mix.wav");
    AudioLooper::ExportOptions opts;
    opts.applyLimiter = false;
    ASSERT_TRUE(looper.exportMix(mix.str().c_str(), opts));

    ASSERT_TRUE(looper.importTrack(1, mix.str().c_str(), kSR));
    EXPECT_EQ(looper.getTrackLengthFrames(1), 6000);
    EXPECT_TRUE(looper.isTrackActive(1));
}

// 24-bit export writes a valid, readable WAV of the expected shape.
TEST(AudioLooper, Export24BitReadsBack) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    WavTempFile src("wm_24_src.wav");
    ASSERT_TRUE(makeWav(src.str(), /*frames=*/3000, kSR, 0.3f));
    ASSERT_TRUE(looper.importTrack(0, src.str().c_str(), kSR));

    WavTempFile out("wm_24_out.wav");
    AudioLooper::ExportOptions opts;
    opts.bitDepth = wav::BitDepth::PCM_24;
    opts.applyLimiter = false;
    ASSERT_TRUE(looper.exportMix(out.str().c_str(), opts));

    wav::WavData wd = wav::readWav(out.str().c_str());
    EXPECT_EQ(wd.numFrames, 3000);
    EXPECT_EQ(wd.numChannels, 2);
    EXPECT_EQ(wd.sampleRate, kSR);
}
