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
#include "tests/support/TestWait.h"
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

// Both storage backends bound the SAME thing now: reserved RAM. A paged track
// pre-reserves its whole capacity in the chunk pool at prepare time (counted by
// reservedBytes()), and hands the slack back to the OS when a shorter take is
// trimmed — so these budget assertions hold identically for dense and chunked.

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

// Budget is runtime-configurable (prepare reserves capacity in both backends).
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

    // PRESENCIA: se espera a que el worker DESPACHE, no a que pasen 60 ms. Un rato
    // fijo contra un worker que polea cada 15 alcanza en una maquina ociosa y se
    // queda corto en un runner cargado — la clase de REQ-002.
    const bool arrived = wma_test::waitUntil([&] { return completed.load() >= 1; });
    looper.setEventDispatcher(nullptr);
    dispatcher.stop();

    EXPECT_TRUE(arrived) << "el evento de track completado nunca llego al worker";
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

// Import must honour the memory budget: with the budget nearly full (a prepared
// track reserves its capacity in both backends), importing a track that doesn't
// fit fails cleanly and leaves the target track empty.
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

// Max |sample| across a WAV — used by the guard test to tell a pure snapshot from
// an overdub-contaminated one.
static float wavMaxAbs(const std::string& path) {
    wav::WavData wd = wav::readWav(path.c_str());
    float m = 0.0f;
    for (float s : wd.buffer) m = std::max(m, std::fabs(s));
    return m;
}

// ExportGuard: while an export is in flight, process() must skip destructive
// overdub writes so the render sees an immutable snapshot (plan §4.1). We hold an
// overdub active on a playing track and hammer process() from another thread for
// the whole duration of a deliberately large export; the exported mix must still
// contain only the ORIGINAL content. Then, with the guard released, the same
// overdub DOES change the track — proving the guard (not a broken overdub) is what
// held the snapshot still. Storage-agnostic: runs under both backends.
TEST(AudioLooper, ExportGuardBlocksOverdubDuringSnapshot) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    const int L = 48000;                          // 1 s loop
    WavTempFile src("wm_guard_src.wav");
    ASSERT_TRUE(makeWav(src.str(), L, kSR, 0.5f));
    ASSERT_TRUE(looper.importTrack(0, src.str().c_str(), kSR));
    looper.resumeTrack(0);                         // playhead advances during export

    // Pure 0.5 track at default pan/volume exports at ~0.5*0.7071 ≈ 0.354. An
    // overdub (input 0.9, gain 0.8, decay 0) pushes the stored sample past 0.7, so
    // the export would exceed 0.45. 0.45 cleanly separates the two cases.
    constexpr float kPureCeil = 0.45f;

    AudioLooper::ExportOptions opts;
    opts.applyLimiter = false;
    opts.repeatLoops = 60;                         // ~2.88M frames → export takes a while

    // Run the export on a background thread while hammering process() (which would
    // overdub if the guard didn't block it). gtest macros stay on the main thread.
    looper.startOverdub(0);
    WavTempFile guarded("wm_guard_during.wav");
    std::atomic<bool> exportDone{false};
    std::atomic<bool> exportOk{false};
    std::thread exporter([&] {
        exportOk.store(looper.exportMix(guarded.str().c_str(), opts));
        exportDone.store(true);
    });
    std::vector<float> blk(512 * 2, 0.9f);
    while (!exportDone.load()) {
        if (looper.isExportInProgress()) looper.process(blk.data(), 512);
        else                             std::this_thread::yield();
    }
    exporter.join();

    EXPECT_TRUE(exportOk.load());
    EXPECT_LT(wavMaxAbs(guarded.str()), kPureCeil)
        << "overdub bled into the export snapshot — guard not holding";

    // Guard released: the SAME overdub now mutates the track (a full pass), so a
    // fresh export shows the elevated level.
    for (int i = 0; i < L * 2; i += 512) looper.process(blk.data(), 512);
    WavTempFile after("wm_guard_after.wav");
    ASSERT_TRUE(looper.exportMix(after.str().c_str(), opts));
    EXPECT_GT(wavMaxAbs(after.str()), kPureCeil)
        << "overdub never took effect even without the guard";
}

// cancelExport aborts an in-flight export: the render bails at the next
// checkpoint, exportMix returns false, and no file is written (plan §4.1).
TEST(AudioLooper, CancelExportMidwayWritesNothing) {
    AudioLooper looper;
    looper.setSampleRate(kSR);

    WavTempFile src("wm_cancel_src.wav");
    ASSERT_TRUE(makeWav(src.str(), 48000, kSR, 0.4f));
    ASSERT_TRUE(looper.importTrack(0, src.str().c_str(), kSR));

    AudioLooper::ExportOptions opts;
    opts.applyLimiter = false;
    opts.repeatLoops = 120;                        // large mix → cancel lands mid-render

    WavTempFile out("wm_cancel_out.wav");
    std::atomic<bool> result{true};
    std::atomic<bool> done{false};
    std::thread exporter([&] {
        result.store(looper.exportMix(out.str().c_str(), opts));
        done.store(true);
    });
    // ExportGuard clears the cancel flag once at start, so a single call could be
    // missed — keep asserting it until the export returns.
    while (!done.load()) {
        if (looper.isExportInProgress()) looper.cancelExport();
        std::this_thread::yield();
    }
    exporter.join();

    EXPECT_FALSE(result.load()) << "cancelled export must report failure";
    EXPECT_FALSE(std::filesystem::exists(out.path))
        << "cancelled export must not leave a partial file";
}

// ======================= Quantized sync-arm =======================
//
// armSyncedToLoop(quantumFrames > 0): capture starts at the next multiple of
// `quantumFrames` (e.g. the next bar) inside the reference cycle instead of the
// next loop wrap, so a punch-in can begin at any moment of the current loop.
// The rotated start offset is cancelled at finalize (finalizeLoopStartPlayback),
// so the take still phase-locks to the reference.

namespace {

// Record a full fixed-length take into `track` and finalize it (playing).
int64_t seedPlayingLoop(AudioLooper& looper, int track, int frames, int64_t pf) {
    EXPECT_TRUE(looper.prepareTrack(track, frames, kSR));
    looper.startRecording(track);
    pf = feed(looper, 0.5f, frames, 256, pf);
    looper.stopRecording();   // loop already closed at the boundary; ends the tail
    EXPECT_TRUE(looper.getTrack(track).isTrackPlaying());
    return pf;
}

// Circular distance between two positions on a loop of length `len`.
int circularDist(int a, int b, int len) {
    int d = std::abs(a - b) % len;
    return std::min(d, len - d);
}

}  // namespace

// Legacy contract (quantum = 0): the trigger still waits for the loop wrap.
TEST(AudioLooper, SyncArmDefaultStillWaitsForTheWrap) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.prepareMixBuffer(512);

    const int bar = 12'000;
    const int refLen = 4 * bar;
    int64_t pf = seedPlayingLoop(looper, 0, refLen, 0);

    // Advance into the middle of the cycle.
    pf = feed(looper, 0.0f, bar + bar / 2, 256, pf);
    const int refPos = looper.getTrack(0).getPlayHead() % refLen;

    ASSERT_TRUE(looper.prepareTrack(1, refLen, kSR));
    const int64_t trigger = looper.armSyncedToLoop(1, pf, /*latencyFrames=*/0);
    ASSERT_GE(trigger, 0);
    EXPECT_EQ(trigger - pf, static_cast<int64_t>(refLen - refPos))
        << "default sync-arm must keep waiting for the wrap";
    looper.cancelArm();
}

// Quantized: the trigger lands on the NEXT BAR (short wait), and after the take
// closes its loop, the rotated content still plays phase-locked to the reference.
TEST(AudioLooper, QuantizedSyncArmStartsAtNextBarAndPhaseLocks) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.prepareMixBuffer(512);

    const int bar = 12'000;
    const int refLen = 4 * bar;
    int64_t pf = seedPlayingLoop(looper, 0, refLen, 0);

    // Advance partway into bar 2 so the wrap is still ~2.5 bars away.
    pf = feed(looper, 0.0f, bar + bar / 2, 256, pf);
    const int refPosAtArm = looper.getTrack(0).getPlayHead() % refLen;

    ASSERT_TRUE(looper.prepareTrack(1, refLen, kSR));
    const int64_t trigger =
        looper.armSyncedToLoop(1, pf, /*latencyFrames=*/0, /*quantumFrames=*/bar);
    ASSERT_GE(trigger, 0);

    const int wait = static_cast<int>(trigger - pf);
    EXPECT_GT(wait, 0);
    EXPECT_LE(wait, bar) << "quantized arm must start within one bar";
    EXPECT_LT(wait, refLen - refPosAtArm)
        << "quantized arm must start strictly before the loop wrap";
    // Where in the cycle the take will start capturing (its buffer frame 0).
    const int startOffset = (refPosAtArm + wait) % refLen;
    EXPECT_EQ(startOffset % bar, 0) << "capture must start on a bar boundary";

    // Reach the trigger, capture a full take, then end the tail phase.
    pf = feed(looper, 0.25f, wait, 256, pf);
    pf = feed(looper, 0.25f, refLen + 512, 256, pf);   // loop closes at refLen
    looper.stopRecording();
    ASSERT_TRUE(looper.getTrack(1).isTrackPlaying());

    // Let both tracks run a little, then check the lock: the take's position must
    // equal the reference's position shifted back by the start offset (mod len).
    pf = feed(looper, 0.0f, 4096, 256, pf);
    const int refPos = looper.getTrack(0).getPlayHead() % refLen;
    const int takePos = looper.getTrack(1).getPlayHead() % refLen;
    int expected = (refPos - startOffset) % refLen;
    if (expected < 0) expected += refLen;
    EXPECT_LE(circularDist(takePos, expected, refLen), 512)
        << "rotated take must phase-lock to the reference (refPos=" << refPos
        << " startOffset=" << startOffset << " takePos=" << takePos << ")";
}
