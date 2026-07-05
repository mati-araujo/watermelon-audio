#pragma once

#include "TrackBuffer.h"
#include "WavFile.h"
#include "Limiter.h"
#include "PanLUT.h"
#include "LooperEventDispatcher.h"
#include "LooperStateEmitter.h"
#include "MetronomeClick.h"
#include "LooperExportTypes.h"
#include "../dsp/SIMDUtils.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include "../platform/Logger.h"

#define LOOPER_LOG_TAG "Looper"
#define LOOPER_LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, LOOPER_LOG_TAG, __VA_ARGS__)
#define LOOPER_LOGI(...) wma::logMessage(wma::LogLevel::INFO,  LOOPER_LOG_TAG, __VA_ARGS__)
#define LOOPER_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOOPER_LOG_TAG, __VA_ARGS__)

// Offline WAV export/import lives in LooperExporter.{h,cpp} (friend of this
// class) so the ~500 lines of IO/resample code stay out of every consumer's TU.
namespace wm { class LooperExporter; }

/**
 * @class AudioLooper
 * @brief Multi-track audio looper for live performance
 *
 * Captures audio from the output stage and plays it back in a loop.
 * Supports up to 8 simultaneous tracks with independent volume/mute/pan.
 *
 * Architecture:
 * - process() is called from the audio thread callback (RT-safe)
 * - Control methods (prepare, start, stop, clear) called from UI thread
 * - Communication via std::atomic (no locks in audio thread)
 * - Each track has its own independent playhead and loop length
 * - Tracks can be played, paused, and deleted independently
 *
 * Memory:
 * - 48 MB hard cap across all tracks
 * - Buffers allocated lazily via prepareTrack()
 */
class AudioLooper {
public:
    // Compile-time hardware ceiling: internal arrays are sized to this so a
    // device tier can raise the active track count at runtime without a rebuild
    // (plan §3.2). MAX_TRACKS is kept as a back-compat alias (JNI / index bounds).
    static constexpr int MAX_TRACKS_HW = 16;
    static constexpr int MAX_TRACKS = MAX_TRACKS_HW;
    static constexpr int    DEFAULT_MAX_ACTIVE_TRACKS = 8;
    static constexpr int    DEFAULT_MAX_FREE_SECONDS  = 60;
    static constexpr size_t DEFAULT_MEMORY_BUDGET_BYTES = 48ULL * 1024ULL * 1024ULL;  // 48 MB
    // Back-compat alias for the default budget (was a hard constant).
    static constexpr size_t MEMORY_BUDGET_BYTES = DEFAULT_MEMORY_BUDGET_BYTES;

    /**
     * @brief Runtime capabilities a device tier configures before/between
     *        sessions (plan §3.2). Defaults reproduce the historical behaviour,
     *        so callers that never set them see no change. NoisyPad picks the
     *        tier (device RAM + Remote Config); the engine stays Android-agnostic.
     */
    struct LooperCapabilities {
        size_t memoryBudgetBytes = DEFAULT_MEMORY_BUDGET_BYTES;
        int    maxActiveTracks   = DEFAULT_MAX_ACTIVE_TRACKS;   // ceiling MAX_TRACKS_HW
        int    maxFreeSeconds    = DEFAULT_MAX_FREE_SECONDS;
        int    chunkPoolPrefill  = 0;   // chunks pre-allocated per track (0 = storage default)
    };

    // Push-based state-change notifications (progress/playing/peak/record) are
    // coalesced and emitted by wm::LooperStateEmitter (see mStateEmitter) —
    // thresholds live there.
    // Initial mix buffer capacity; grows on demand if Oboe gives larger callbacks.
    static constexpr int INITIAL_MIX_CAPACITY_FRAMES = 2048;

    AudioLooper() {
        mLooperMixBuf.resize(static_cast<size_t>(INITIAL_MIX_CAPACITY_FRAMES) * 2, 0.0f);
    }
    ~AudioLooper() = default;

    /**
     * @brief Apply runtime capabilities. UI/control thread, before/between
     *        sessions. Safe-reduction rules (plan §3.2): lowering the budget with
     *        tracks already loaded never frees content — it only affects future
     *        allocations; lowering maxActiveTracks never deactivates a track that
     *        is already active (the limit is clamped up to the highest active
     *        track so the mix loop keeps covering it).
     */
    void setCapabilities(const LooperCapabilities& caps) {
        mMemoryBudgetBytes.store(caps.memoryBudgetBytes > 0 ? caps.memoryBudgetBytes
                                                            : DEFAULT_MEMORY_BUDGET_BYTES,
                                 std::memory_order_release);
        int reqTracks = std::clamp(caps.maxActiveTracks, 1, MAX_TRACKS_HW);
        reqTracks = std::max(reqTracks, highestActiveTrackPlusOne());
        mMaxActiveTracks.store(reqTracks, std::memory_order_release);
        mMaxFreeSeconds.store(std::max(1, caps.maxFreeSeconds), std::memory_order_release);
        mChunkPoolPrefill.store(std::max(0, caps.chunkPoolPrefill), std::memory_order_release);
    }

    LooperCapabilities getCapabilities() const {
        LooperCapabilities c;
        c.memoryBudgetBytes = mMemoryBudgetBytes.load(std::memory_order_acquire);
        c.maxActiveTracks   = mMaxActiveTracks.load(std::memory_order_acquire);
        c.maxFreeSeconds    = mMaxFreeSeconds.load(std::memory_order_acquire);
        c.chunkPoolPrefill  = mChunkPoolPrefill.load(std::memory_order_acquire);
        return c;
    }

    int getMaxActiveTracks() const { return mMaxActiveTracks.load(std::memory_order_acquire); }
    size_t getMemoryBudgetBytes() const { return mMemoryBudgetBytes.load(std::memory_order_acquire); }
    int getMaxFreeSeconds() const { return mMaxFreeSeconds.load(std::memory_order_acquire); }

    /**
     * @brief Update sample rate. Call from UI/control thread (NOT audio thread).
     *        Recomputes click envelope durations.
     */
    void setSampleRate(int sampleRate) {
        if (sampleRate <= 0) return;
        mSampleRate.store(sampleRate, std::memory_order_release);
        mClick.setSampleRate(sampleRate);
    }
    int getSampleRate() const { return mSampleRate.load(std::memory_order_acquire); }

    /**
     * @brief Pre-size the internal mix buffer to the largest audio block Oboe
     *        can deliver. Call from the UI/control thread on prepare()/stream
     *        (re)configuration, passing the max frames-per-callback (typically
     *        framesPerBurst × N). This eliminates the grow-on-demand resize()
     *        that would otherwise run on the audio thread the first time a
     *        callback exceeds INITIAL_MIX_CAPACITY_FRAMES (QW-4). Only grows;
     *        never shrinks. NOT RT-safe — never call from the audio thread.
     */
    void prepareMixBuffer(int maxBlockFrames) {
        if (maxBlockFrames <= 0) return;
        const size_t needed = static_cast<size_t>(maxBlockFrames) * 2;
        if (mLooperMixBuf.size() < needed) {
            mLooperMixBuf.resize(needed, 0.0f);
            LOOPER_LOGI("mix buffer pre-sized to %d frames (UI thread)", maxBlockFrames);
        }
    }

    // ========== Audio thread (RT-safe) ==========

    /**
     * @brief Audio thread tick. The optional `playFrame` parameter is the
     *        Transport's play position at the START of this audio block. It is
     *        used to fire armed recordings exactly at the requested trigger
     *        frame (downbeat-aligned).
     */
    void process(float* audioData, int numFrames, int64_t playFrame = -1) {
        if (!mEnabled.load(std::memory_order_acquire)) {
            mClick.render(audioData, numFrames);
            return;
        }

        // ---- ARMED-RECORDING TRIGGER ----
        // If a track is armed and Transport's play position has reached or
        // passed the trigger frame, transition to recording. We do this first
        // so capture begins on this same audio block.
        int armed = mArmedTrack.load(std::memory_order_acquire);
        if (armed >= 0 && playFrame >= 0) {
            int64_t trigger = mArmedTriggerFrame.load(std::memory_order_acquire);
            if (playFrame + numFrames > trigger) {
                // The trigger falls inside (or before) this block — start now.
                // We accept up to one block of jitter (≤ ~10ms) for now.
                startRecording(armed);
                mArmedTrack.store(-1, std::memory_order_release);
                mArmedTriggered.fetch_add(1, std::memory_order_relaxed);
            }
        }

        int recTrack = mRecordingTrack.load(std::memory_order_acquire);
        bool overdubbing = mOverdubbing.load(std::memory_order_acquire);

        // ---- CAPTURE (recording or overdub) ----
        // Skip ALL destructive writes (overdub, recording) while an export is in
        // progress to avoid mid-snapshot mutation. Playback is read-only and
        // unaffected.
        const bool exportActive = mExportInProgress.load(std::memory_order_acquire);
        if (recTrack >= 0 && recTrack < MAX_TRACKS && !exportActive) {
            int trackLen = mTracks[recTrack].getLengthFrames();

            if (overdubbing && trackLen > 0) {
                // Use the target track's own playhead for overdub position.
                // QW-3: walk the block in contiguous runs up to each loop wrap
                // instead of a modulo per frame. pos stays in [0, trackLen); the
                // run length is bounded by whichever comes first — end of block or
                // the wrap — so a short loop under a large callback still works.
                int playHead = mTracks[recTrack].getPlayHead();
                float gain = mOverdubGain.load(std::memory_order_relaxed);
                float decay = mOverdubDecay.load(std::memory_order_relaxed);
                int pos = playHead % trackLen;   // one modulo total, not per frame
                if (pos < 0) pos += trackLen;
                int i = 0;
                while (i < numFrames) {
                    const int run = std::min(numFrames - i, trackLen - pos);
                    for (int k = 0; k < run; ++k) {
                        mTracks[recTrack].overdubFrame(
                            pos + k, audioData[(i + k) * 2], audioData[(i + k) * 2 + 1],
                            gain, decay);
                    }
                    i += run;
                    pos += run;
                    if (pos >= trackLen) pos = 0;
                }
            } else {
                // Normal recording with circular wrap-mix tail.
                // mRecordCapacityFrames = loopFrames + tailWindow (tailWindow is 0
                // for percussion / free-at-cap; set in startRecording).
                //   1. First `loopFrames` frames are captured linearly into the body.
                //   2. The next `tailWindow` frames — the audio still ringing past the
                //      loop boundary — are OVERDUBBED (mixed, soft-clipped) into the
                //      START of the loop with a decay, so sustained sounds bleed across
                //      the seam (standard looper behavior; mixed, not overwritten).
                // Checkpoints:
                //   1. Write head reaches loopCapacity -> finalize loop, start playback.
                //   2. remaining reaches 0 -> stop recording entirely.
                int capacity = mRecordCapacityFrames.load(std::memory_order_relaxed);
                int remaining = mRecordFramesRemaining.load(std::memory_order_relaxed);
                bool loopFinalized = mLoopFinalizedDuringRec.load(std::memory_order_relaxed);

                const int loopCap = mTracks[recTrack].getLoopCapacityFrames();
                const int tailWindow = capacity - loopCap;   // 0 = no wrap-mix
                const int recordedSoFar = capacity - remaining;
                const float invTail = (tailWindow > 0)
                    ? 1.0f / static_cast<float>(tailWindow) : 0.0f;

                int dropped = 0;
                for (int i = 0; i < numFrames && remaining > 0; ++i) {
                    const float l = audioData[i * 2];
                    const float r = audioData[i * 2 + 1];
                    const int pos = recordedSoFar + i;
                    if (pos < loopCap) {
                        // First pass — linear capture into the loop body.
                        if (!mTracks[recTrack].writeFrame(l, r)) ++dropped;
                    } else if (tailWindow > 0) {
                        // Wrap-mix tail — overdub the ringing continuation into the
                        // loop start with a quasi-exponential decay (1-t)^2.
                        const int tailIdx = pos - loopCap;       // 0..tailWindow-1
                        float fade = 1.0f - static_cast<float>(tailIdx) * invTail;
                        fade = fade * fade;
                        mTracks[recTrack].overdubFrame(tailIdx, l, r, fade, 0.0f);
                    }
                    remaining--;
                }
                mRecordFramesRemaining.store(remaining, std::memory_order_relaxed);
                if (dropped > 0) {
                    mFramesDropped.fetch_add(dropped, std::memory_order_relaxed);
                }

                // Update recording progress relative to the MUSICAL loop (0..1),
                // not total capacity. With a tail of 750ms appended after the loop,
                // total capacity exceeds the bar-aligned loop length by ~15-20% —
                // dividing by capacity drifts the progress bar off-beat. After the
                // loop boundary is crossed, progress sits at 1.0 while the tail
                // finishes capturing in the background, then recording stops.
                const int musicalLoop = mMusicalLoopFrames.load(std::memory_order_relaxed);
                if (musicalLoop > 0 && capacity > 0) {
                    const int written = capacity - remaining;
                    float recProg = static_cast<float>(written) / static_cast<float>(musicalLoop);
                    mRecordProgress.store(std::min(recProg, 1.0f), std::memory_order_relaxed);
                }

                // Checkpoint 1: loop boundary crossed → start playback.
                if (!loopFinalized && mTracks[recTrack].hasReachedLoopEnd()) {
                    finalizeLoopStartPlayback(recTrack);
                    mLoopFinalizedDuringRec.store(true, std::memory_order_release);
                }

                // Checkpoint 2: total buffer capacity reached → stop recording.
                // Applies in BOTH fixed and free-length modes: a free take can't
                // exceed its pre-sized buffer, so when the buffer fills we must
                // finalize and clear mRecordingTrack. Previously this was guarded
                // by !mFreeLength, which left isRecording() stuck true forever once
                // a free take hit the cap — the record button then stayed disabled.
                if (remaining <= 0) {
                    if (!mLoopFinalizedDuringRec.load(std::memory_order_relaxed)) {
                        // No tail captured (tail==0) — finalize loop now.
                        finalizeLoopStartPlayback(recTrack);
                    }
                    // Recording done (loop + tail).
                    mRecordingTrack.store(-1, std::memory_order_release);
                    mLoopFinalizedDuringRec.store(false, std::memory_order_release);
                }
            }
        }

        // ---- PLAYBACK (mix all tracks into temp buffer, apply master volume) ----
        // mLooperMixBuf grows on demand if Oboe delivers larger callbacks than the initial
        // capacity. The growth path runs on the audio thread, but is rare in steady state
        // (only first oversized callback triggers it). After grow, subsequent callbacks reuse.
        const size_t needed = static_cast<size_t>(numFrames) * 2;
        if (mLooperMixBuf.capacity() < needed) {
            // FALLBACK ONLY: prepareMixBuffer() should have pre-sized this from the
            // UI thread. Reaching here means a callback exceeded the pre-sized max
            // (or prepareMixBuffer was never called) — a real-time alloc we can't
            // avoid without dropping audio. Log at ERROR so it surfaces.
            mLooperMixBuf.resize(needed);
            LOOPER_LOGE("mix buffer grown ON AUDIO THREAD to %d frames — prepareMixBuffer "
                        "under-sized; RT alloc occurred", numFrames);
        }
        std::memset(mLooperMixBuf.data(), 0, sizeof(float) * needed);

        // Only iterate up to the active-track limit — saves CPU on low tiers, and
        // the limit is never below the highest active track (setCapabilities keeps
        // that invariant), so no active track is ever skipped.
        const int maxActive = mMaxActiveTracks.load(std::memory_order_relaxed);
        for (int t = 0; t < maxActive; ++t) {
            mTracks[t].mixInto(mLooperMixBuf.data(), numFrames);
        }

        // Apply master volume + accumulate into main output.
        // We replace the per-sample smoothing+accumulate scalar loop with two
        // SIMD passes: a linear gain ramp on the mix buffer followed by a
        // SIMD add into the main output. This matches the steady-state behavior
        // of the previous one-pole smoother to within ~0.1% (per-block linear
        // ramp instead of exponential smoothing) but is 2-4x faster on NEON.
        //
        // We compute gainStart from the previous block's smoother state and
        // gainEnd by simulating the smoother across `numFrames` samples,
        // preserving the smoother's continuity across audio blocks.
        float masterVolStart = mMasterVolSmoother.load(std::memory_order_relaxed);
        const float targetMaster = mMasterVolume.load(std::memory_order_acquire);
        constexpr float kSmooth = 0.995f;
        // Closed-form per-block end value of the one-pole smoother:
        //   y_n = α^n * y_0 + (1-α^n) * target
        const float alphaPow = std::pow(kSmooth, static_cast<float>(numFrames));
        const float masterVolEnd = alphaPow * masterVolStart
                                 + (1.0f - alphaPow) * targetMaster;

        // Apply linear ramp to the mix buffer in-place (SIMD).
        simd::applyStereoGainRamp(mLooperMixBuf.data(), numFrames,
                                  masterVolStart, masterVolEnd);
        // Accumulate mix into output (SIMD). applyHeadroom=false → straight sum
        // (we don't want the default -6dB headroom; master vol already applied).
        simd::addStereoBuffers(audioData, audioData, mLooperMixBuf.data(), numFrames,
                               /*applyHeadroom=*/false);

        mMasterVolSmoother.store(masterVolEnd, std::memory_order_relaxed);

        mClick.render(audioData, numFrames);

        // ---- STATE-CHANGE NOTIFICATIONS (push-based) ----
        // Compare each track's observable state against the last-emitted
        // snapshot; push a LooperEvent when a threshold is crossed. Lock-free
        // and RT-safe — the dispatcher's queue absorbs jitter, a worker thread
        // drains it and invokes the Kotlin listener off the audio thread.
        if (mDispatcher) {
            mStateEmitter.emit(mTracks, mMaxActiveTracks.load(std::memory_order_relaxed),
                               mDispatcher,
                               mRecordingTrack.load(std::memory_order_acquire),
                               mRecordProgress.load(std::memory_order_relaxed));
        }
    }

    /**
     * @brief Register the dispatcher that the audio thread will push state
     *        events to. Set ONCE from the owning AudioEngine at construction
     *        time, before audio callbacks start. Pass nullptr to disable.
     *        Not thread-safe wrt audio thread — must be done before start.
     */
    void setEventDispatcher(wm::LooperEventDispatcher* dispatcher) {
        mDispatcher = dispatcher;
    }

    // ========== Control (UI thread) ==========

    /**
     * @brief Prepare a track sized to N musical bars at the current Transport
     *        BPM/beats-per-bar/sample rate. Convenience wrapper around prepareTrack.
     * @param trackIndex 0..MAX_TRACKS-1
     * @param bars Number of bars (>=1)
     * @param framesPerBar Frames-per-bar from Transport::framesPerBar(bars=1).
     *        Caller computes via Transport so we keep AudioLooper free of the
     *        Transport include and avoid header coupling.
     * @return true if allocated successfully
     */
    bool prepareTrackBars(int trackIndex, int bars, int framesPerBar, int sampleRate) {
        if (bars <= 0 || framesPerBar <= 0) return false;
        return prepareTrack(trackIndex, bars * framesPerBar, sampleRate);
    }

    bool prepareTrack(int trackIndex, int lengthFrames, int sampleRate) {
        // Reject tracks beyond the active-track limit (device tier), not just the
        // hardware ceiling — a low tier exposes fewer usable tracks.
        if (trackIndex < 0 || trackIndex >= mMaxActiveTracks.load(std::memory_order_acquire))
            return false;
        if (lengthFrames <= 0) return false;

        // Wrap-mix tail window (in frames): the ringing continuation past the loop
        // boundary is overdubbed INTO the loop start at record time, so no extra
        // buffer is allocated for it — the window is just metadata. tailMs is
        // global (see setTailMs); TrackBuffer caps it at loopFrames.
        const int tailMs = mTailMs.load(std::memory_order_acquire);
        const int tailFrames = (tailMs > 0)
            ? (tailMs * sampleRate) / 1000
            : 0;

        // Buffer is exactly the loop body (no separate tail region anymore).
        size_t needed = static_cast<size_t>(lengthFrames) * 2 * sizeof(float);
        size_t currentUsage = getTotalAllocatedBytes();
        size_t trackCurrent = mTracks[trackIndex].allocatedBytes();
        if (currentUsage - trackCurrent + needed
                > mMemoryBudgetBytes.load(std::memory_order_acquire)) {
            return false;
        }

        size_t allocated = mTracks[trackIndex].allocate(lengthFrames, sampleRate, tailFrames);
        return allocated > 0;
    }

    /**
     * @brief Set the tail capture length in milliseconds (default 250ms).
     *        Affects all tracks prepared AFTER this call (existing tracks unchanged).
     *        Set to 0 to disable tail capture (legacy behavior).
     *        Capped at 2000 ms.
     */
    void setTailMs(int ms) {
        if (ms < 0) ms = 0;
        if (ms > 2000) ms = 2000;
        mTailMs.store(ms, std::memory_order_release);
    }
    int getTailMs() const { return mTailMs.load(std::memory_order_acquire); }

    void startRecording(int trackIndex) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        const int loopCap = mTracks[trackIndex].getLoopCapacityFrames();
        if (loopCap <= 0) return;
        // Total frames to capture = loop body + wrap-mix tail window. The tail is
        // skipped for percussion tracks (hard seam, no bleed). The body is captured
        // linearly; the tail overdubs into the loop start (see process()).
        // Wrap-mix window never exceeds half the loop, so the baked decay can't
        // swamp the loop start on short loops.
        const int tailWindow = mTracks[trackIndex].isPercussionMode()
            ? 0 : std::min(mTracks[trackIndex].getTailFrames(), loopCap / 2);
        const int recordTotal = loopCap + tailWindow;
        mRecordCapacityFrames.store(recordTotal, std::memory_order_release);
        mRecordFramesRemaining.store(recordTotal, std::memory_order_release);
        // Musical loop length (excludes tail) — used by recording progress so the
        // bar fills in lockstep with the bar boundary, not the post-tail boundary.
        mMusicalLoopFrames.store(loopCap, std::memory_order_release);
        mRecordProgress.store(0.0f, std::memory_order_release);
        mLoopFinalizedDuringRec.store(false, std::memory_order_release);
        mOverdubbing.store(false, std::memory_order_release);
        mRecordingTrack.store(trackIndex, std::memory_order_release);
        mEnabled.store(true, std::memory_order_release);
    }

    /**
     * @brief Start recording with a pre-roll seed: writes `preRollFrames` of
     *        prior audio into the start of the track, then continues capturing
     *        live audio from the audio thread. Call from UI thread; the
     *        seed write is one-shot and finishes before recording begins.
     * @param trackIndex Target track.
     * @param preRollData Stereo interleaved buffer (size = preRollFrames * 2).
     * @param preRollFrames Number of pre-roll frames (clamped to track capacity-1).
     */
    void startRecordingWithPreRoll(int trackIndex, const float* preRollData, int preRollFrames) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        const int capacity = mTracks[trackIndex].getCapacityFrames();
        if (capacity <= 0) return;
        if (preRollFrames < 0) preRollFrames = 0;
        if (preRollFrames >= capacity) preRollFrames = capacity - 1;

        // Write the pre-roll into the buffer head BEFORE flipping the recording
        // flag so the audio thread doesn't race with our seed writes.
        if (preRollData && preRollFrames > 0) {
            for (int i = 0; i < preRollFrames; ++i) {
                mTracks[trackIndex].writeFrame(preRollData[i * 2], preRollData[i * 2 + 1]);
            }
        }

        // Now arm the audio thread to fill the remaining capacity.
        mRecordCapacityFrames.store(capacity, std::memory_order_release);
        mRecordFramesRemaining.store(capacity - preRollFrames, std::memory_order_release);
        mMusicalLoopFrames.store(mTracks[trackIndex].getLoopCapacityFrames(),
                                 std::memory_order_release);
        // Progress already accounts for the pre-roll seed.
        const float seedProgress = (capacity > 0)
            ? static_cast<float>(preRollFrames) / static_cast<float>(capacity)
            : 0.0f;
        mRecordProgress.store(seedProgress, std::memory_order_release);
        mLoopFinalizedDuringRec.store(false, std::memory_order_release);
        mOverdubbing.store(false, std::memory_order_release);
        mRecordingTrack.store(trackIndex, std::memory_order_release);
        mEnabled.store(true, std::memory_order_release);
    }

    /**
     * @brief Arm a track to start recording when the transport reaches
     *        `triggerFrame`. Used for downbeat-aligned multi-track sync.
     *        Caller computes triggerFrame via Transport::nextBarBoundary().
     *        Pass triggerFrame=0 to record immediately on next callback.
     */
    void armRecording(int trackIndex, int64_t triggerFrame) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        if (mTracks[trackIndex].getCapacityFrames() <= 0) return;
        mSyncRefTrack.store(-1, std::memory_order_release);  // plain arm = no phase-lock
        mArmedTriggerFrame.store(triggerFrame, std::memory_order_release);
        mArmedTrack.store(trackIndex, std::memory_order_release);
        mEnabled.store(true, std::memory_order_release);
    }

    /**
     * @brief Arm `trackIndex` to begin recording when the loop reference (the
     *        longest active & playing track) next reaches its loop boundary, plus
     *        `latencyFrames` of round-trip compensation. Phase-locks an overdub
     *        layer to the existing loop:
     *          - the trigger lands `latencyFrames` after the reference's next
     *            loop-zero, so the user's downbeat (recorded late by the round
     *            trip) is captured at the new track's frame 0; and
     *          - at finalize the new track's playhead is set to the reference's
     *            playhead (see finalizeLoopStartPlayback) so the two loops play
     *            in phase.
     *        Assumes the reference plays at speed 1.0 and the new take's length is
     *        equal to (or a multiple of) the reference for a drift-free lock.
     * @param trackIndex   Track to record into (already prepared).
     * @param playFrameNow Transport play position at call time (passed by caller
     *                     so AudioLooper stays free of the Transport include).
     * @param latencyFrames Round-trip latency compensation in frames (>=0).
     * @return the absolute trigger frame, or -1 if no reference track is playing
     *         (caller should fall back to a non-synced arm).
     */
    int64_t armSyncedToLoop(int trackIndex, int64_t playFrameNow, int latencyFrames) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return -1;
        if (mTracks[trackIndex].getCapacityFrames() <= 0) return -1;
        if (latencyFrames < 0) latencyFrames = 0;

        int refIdx = -1;
        int refLen = 0;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (i == trackIndex) continue;
            if (mTracks[i].isActive() && mTracks[i].isTrackPlaying()) {
                const int len = mTracks[i].getLoopLength();
                if (len > refLen) { refLen = len; refIdx = i; }
            }
        }
        if (refIdx < 0 || refLen <= 0) return -1;

        int playhead = mTracks[refIdx].getPlayHead();
        if (playhead < 0) playhead = 0;
        int framesToWrap = refLen - (playhead % refLen);
        if (framesToWrap <= 0) framesToWrap = refLen;  // already at boundary → next loop

        const int64_t trigger = playFrameNow
                              + static_cast<int64_t>(framesToWrap)
                              + static_cast<int64_t>(latencyFrames);

        mSyncRefTrack.store(refIdx, std::memory_order_release);
        mArmedTriggerFrame.store(trigger, std::memory_order_release);
        mArmedTrack.store(trackIndex, std::memory_order_release);
        mEnabled.store(true, std::memory_order_release);
        return trigger;
    }

    /** Cancel any pending armed recording (does not affect a recording in progress). */
    void cancelArm() {
        mArmedTrack.store(-1, std::memory_order_release);
        mSyncRefTrack.store(-1, std::memory_order_release);
    }

    int getArmedTrack() const {
        return mArmedTrack.load(std::memory_order_acquire);
    }

    /**
     * @brief Stop recording. In free-length mode, finalizes at current position.
     */
    void stopRecording() {
        int recTrack = mRecordingTrack.load(std::memory_order_acquire);
        if (recTrack >= 0 && recTrack < MAX_TRACKS) {
            if (!mOverdubbing.load(std::memory_order_acquire)) {
                finalizeCurrentRecording();
            } else {
                // Stop overdub
                mRecordingTrack.store(-1, std::memory_order_release);
                mOverdubbing.store(false, std::memory_order_release);
            }
        }
    }

    /**
     * @brief Abort the in-progress recording (if any) WITHOUT committing it.
     *
     * Unlike stopRecording() — which finalizes the take and starts playback —
     * abortRecording() throws away whatever has been captured so far and
     * leaves the target track in the idle/empty state. Used by scene-change
     * flows that would otherwise capture fade-out / FX-transition / fade-in
     * into the take.
     *
     * Safe to call when no recording is in progress (no-op).
     *
     * Lock-free / RT-safe: flips atomic flags then clears the track buffer.
     * Track::clear() resets play state and zeros the buffer (no allocation).
     */
    void abortRecording() {
        const int recTrack = mRecordingTrack.load(std::memory_order_acquire);
        const int armed = mArmedTrack.load(std::memory_order_acquire);

        // Always clear any armed-recording so a pending take doesn't fire
        // moments after the abort.
        mArmedTrack.store(-1, std::memory_order_release);
        mSyncRefTrack.store(-1, std::memory_order_release);

        if (recTrack < 0 || recTrack >= MAX_TRACKS) {
            // Nothing was recording, but we may have cleared an armed slot.
            if (armed >= 0) {
                LOOPER_LOGI("abortRecording: cleared armed track %d", armed);
            }
            return;
        }

        // Stop the audio thread from writing further samples to the buffer.
        // The audio thread reads mRecordingTrack with acquire ordering, so
        // the next callback after this store will skip the capture branch.
        mRecordingTrack.store(-1, std::memory_order_release);
        mOverdubbing.store(false, std::memory_order_release);
        mLoopFinalizedDuringRec.store(false, std::memory_order_release);
        mRecordFramesRemaining.store(0, std::memory_order_release);
        mRecordProgress.store(0.0f, std::memory_order_release);

        // Discard any partial content. clear() zeros the buffer and resets
        // play state — the track returns to "empty / idle". Note: if this
        // was an overdub on an already-finalized track, clear() also wipes
        // the previously committed loop. That's the desired semantics for
        // a scene change: callers must decide before calling whether to
        // preserve overdub bases (current handleLoadScene path discards).
        mTracks[recTrack].clear();

        LOOPER_LOGI("abortRecording: discarded take on track %d", recTrack);
    }

    void startOverdub(int trackIndex) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        if (!mTracks[trackIndex].isActive()) return;
        mOverdubbing.store(true, std::memory_order_release);
        mRecordingTrack.store(trackIndex, std::memory_order_release);
    }

    /** Pause playback — keeps playhead positions, keeps tracks. */
    void pause() {
        for (int i = 0; i < MAX_TRACKS; ++i) {
            mTracks[i].setPlaying(false);
        }
    }

    /** Resume playback from current playhead positions. */
    void resume() {
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isActive()) {
                mTracks[i].setPlaying(true);
            }
        }
    }

    /** Stop everything — stops recording + pauses playback. Does NOT clear tracks. */
    void stopAll() {
        int recTrack = mRecordingTrack.load(std::memory_order_acquire);
        if (recTrack >= 0 && recTrack < MAX_TRACKS && !mOverdubbing.load(std::memory_order_acquire)) {
            finalizeCurrentRecording();
        }
        mRecordingTrack.store(-1, std::memory_order_release);
        mArmedTrack.store(-1, std::memory_order_release);
        mSyncRefTrack.store(-1, std::memory_order_release);
        mOverdubbing.store(false, std::memory_order_release);
        for (int i = 0; i < MAX_TRACKS; ++i) {
            mTracks[i].setPlaying(false);
        }
    }

    void clearTrack(int trackIndex) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        if (mRecordingTrack.load(std::memory_order_acquire) == trackIndex) {
            mRecordingTrack.store(-1, std::memory_order_release);
        }
        mTracks[trackIndex].clear();  // clear() resets mPlaying, mPlayHead, mProgress
    }

    /**
     * @brief Trim a track's buffer to its recorded length (frees unused capacity).
     *        UI/IO thread only — NOT RT-safe. No-op if recording into this track.
     *        Primarily used after a free-length take so the pre-sized 60s buffer
     *        is released back to the memory budget. Returns true if trimmed.
     */
    bool trimTrack(int trackIndex) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return false;
        if (mRecordingTrack.load(std::memory_order_acquire) == trackIndex) return false;
        if (mExportInProgress.load(std::memory_order_acquire)) return false;
        return mTracks[trackIndex].trimToLength();
    }

    void clearAll() {
        mRecordingTrack.store(-1, std::memory_order_release);
        mArmedTrack.store(-1, std::memory_order_release);
        mSyncRefTrack.store(-1, std::memory_order_release);
        mOverdubbing.store(false, std::memory_order_release);
        for (int i = 0; i < MAX_TRACKS; ++i) {
            mTracks[i].clear();  // clear() resets mPlaying, mPlayHead, mProgress
        }
        mRecordProgress.store(0.0f, std::memory_order_release);
    }

    // ========== Per-track playback control (lock-free) ==========

    void pauseTrack(int index) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].setPlaying(false);
    }
    void resumeTrack(int index) {
        if (index >= 0 && index < MAX_TRACKS && mTracks[index].isActive()) {
            mTracks[index].setPlaying(true);
        }
    }
    bool isTrackPlaying(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return false;
        return mTracks[index].isTrackPlaying();
    }
    float getTrackProgress(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return 0.0f;
        return mTracks[index].getProgress();
    }
    int getTrackLengthFrames(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return 0;
        return mTracks[index].getLengthFrames();
    }
    void resetTrackPlayHead(int index) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].resetPlayHead();
    }
    bool saveUndoSnapshot(int index) {
        if (index < 0 || index >= MAX_TRACKS) return false;
        return mTracks[index].saveUndoSnapshot();
    }
    bool restoreUndo(int index) {
        if (index < 0 || index >= MAX_TRACKS) return false;
        return mTracks[index].restoreUndo();
    }
    bool hasUndo(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return false;
        return mTracks[index].hasUndo();
    }

    // ========== Export / Import (NOT RT-safe — call from IO thread) ==========
    //
    // Implementations live in LooperExporter.cpp (friend). The API surface here
    // is unchanged; only the bodies moved out of the header.

    // Backward-compat alias so existing callers keep using AudioLooper::ExportOptions.
    using ExportOptions = wm::ExportOptions;

    /**
     * @brief Export mix of all active tracks to a WAV file.
     *
     * Uses an "export guard" (mExportInProgress) that the audio thread checks
     * before running overdub or finalize, so the buffer contents read during
     * export cannot be mutated mid-snapshot. Playback continues normally during
     * export. clear()/importTrack() callers are expected to also respect the
     * guard (enforced by JNI serialization on the UI/IO thread).
     *
     * @param filePath Output file path.
     * @param opts     Export options (bit depth, repeat, count-in, limiter, metadata).
     * @return true if successful.
     */
    bool exportMix(const char* filePath, const ExportOptions& opts);

    /** Backward-compat: defaults (16-bit, 1 loop, no count-in, limiter on). */
    bool exportMix(const char* filePath) {
        return exportMix(filePath, ExportOptions{});
    }

    /**
     * @brief Export each active track as a separate WAV file inside `directory`.
     *        Files are named "track_<idx>.wav" (idx is the original track index).
     *        All files share the same length (= longest track * repeatLoops) and
     *        bit depth, so they can be loaded into a DAW for external mixing.
     *
     * @param directory Output directory (must exist; trailing slash optional).
     * @param opts      Same options struct as exportMix. limiter is applied
     *                  PER STEM, not across the bus, so individual track peaks
     *                  are protected without inter-track gain interaction.
     * @return Number of stems written, or -1 on failure.
     */
    int exportStems(const char* directory, const ExportOptions& opts);

    /**
     * @brief Export a single track to a WAV file.
     * @param trackIndex Track to export (0-7)
     * @param filePath   Output file path
     * @param opts       Export options (defaults: 16-bit, no metadata, no limiter
     *                   to preserve original dynamics for DAW import).
     * @return true if successful
     */
    bool exportTrack(int trackIndex, const char* filePath, const ExportOptions& opts);

    /** Backward-compat overload. */
    bool exportTrack(int trackIndex, const char* filePath) {
        return exportTrack(trackIndex, filePath, ExportOptions{});
    }

    /**
     * @brief Session capture: write the FULL track buffer to a WAV.
     *
     * Unlike [exportTrack], this ignores any active loop region — the entire
     * recorded buffer is written so a session save/restore cycle is lossless
     * (the loop region is persisted as metadata by the caller and re-applied on
     * restore). Use [wav::BitDepth::FLOAT_32] for bit-exact round-trips.
     *
     * @param trackIndex Track to capture (0-7).
     * @param filePath   Output WAV path.
     * @param bitDepth   16/24-bit PCM or 32-bit float (float = lossless).
     * @return true if successful.
     */
    bool captureTrack(int trackIndex, const char* filePath, wav::BitDepth bitDepth);

    // ========== Export progress / cancel ==========

    /** [0..1] progress of last/in-flight export. Lock-free. */
    float getExportProgress() const {
        return mExportProgress.load(std::memory_order_acquire);
    }

    /** Set the cancel flag. exportMix/exportStems will bail at the next iteration. */
    void cancelExport() {
        mCancelExport.store(true, std::memory_order_release);
    }

    /**
     * Target sample rate for subsequent exports (0 = engine rate). When nonzero
     * and different from the engine rate, exportMix/exportStems resample the
     * rendered mix to this rate — e.g. 44100 for DAWs that default to 44.1 kHz.
     * WAV / stems path only; the caller keeps the engine rate for compressed export.
     */
    void setExportSampleRate(int sampleRate) {
        mExportSampleRate.store(sampleRate > 0 ? sampleRate : 0, std::memory_order_release);
    }

    bool isExportInProgress() const {
        return mExportInProgress.load(std::memory_order_acquire);
    }

    // ========== Telemetry (lock-free counters) ==========
    //
    // Diagnostic metrics for runtime observability. All counters are monotonic
    // and use relaxed atomics — they're for human-facing dashboards, not
    // synchronization. Reset with resetTelemetry() (UI thread).

    /** Total frames the audio thread tried to record but couldn't (buffer full). */
    int64_t getFramesDropped() const {
        return mFramesDropped.load(std::memory_order_relaxed);
    }
    /** Total exports that finished writing successfully. */
    int64_t getExportsCompleted() const {
        return mExportsCompleted.load(std::memory_order_relaxed);
    }
    /** Total exports that failed (IO error, cancellation, no active tracks). */
    int64_t getExportsFailed() const {
        return mExportsFailed.load(std::memory_order_relaxed);
    }
    /** Total stem files written across all exportStems calls. */
    int64_t getStemsWritten() const {
        return mStemsWritten.load(std::memory_order_relaxed);
    }
    /** Total armed→fired recording transitions (downbeat sync). */
    int64_t getArmedTriggered() const {
        return mArmedTriggered.load(std::memory_order_relaxed);
    }

    void resetTelemetry() {
        mFramesDropped.store(0, std::memory_order_relaxed);
        mExportsCompleted.store(0, std::memory_order_relaxed);
        mExportsFailed.store(0, std::memory_order_relaxed);
        mStemsWritten.store(0, std::memory_order_relaxed);
        mArmedTriggered.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Import a WAV file into a track, replacing its content.
     *        The track must be stopped before importing.
     * @param trackIndex Track to load into (0-7)
     * @param filePath WAV file path
     * @param sampleRate Expected sample rate (for validation)
     * @return true if successful
     */
    bool importTrack(int trackIndex, const char* filePath, int sampleRate);

    // ========== Track parameters (lock-free) ==========

    void setTrackMuted(int index, bool muted) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].setMuted(muted);
    }
    void setTrackVolume(int index, float vol) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].setVolume(vol);
    }
    void setTrackPan(int index, float pan) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].setPan(pan);
    }
    void setTrackSpeed(int index, float speed) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].setSpeed(speed);
    }
    /**
     * @brief Set how many times a track's loop plays before it auto-stops and
     *        emits TrackCompleted (F3.4). n <= 0 = infinite (default). RT-safe.
     */
    void setTrackPlayCount(int index, int plays) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].setPlayCount(plays);
    }
    int getTrackRemainingPlays(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return -1;
        return mTracks[index].getRemainingPlays();
    }
    /** Switch a track's loop-seam profile (true = percussion / hard cut, no tail
     *  bleed; false = sustained / long crossfade + tail). Live & RT-safe. */
    void setTrackPercussionMode(int index, bool percussion) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].setPercussionMode(percussion);
    }
    bool isTrackPercussionMode(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return false;
        return mTracks[index].isPercussionMode();
    }
    /**
     * @brief Get a waveform summary (peak amplitudes) for visualization.
     *
     * While the track is being captured (fresh recording or overdub, audio
     * thread mutating the buffer) we serve the LIVE incremental waveform (QW-6),
     * which is accumulated peak-per-bin as frames are written — race-free
     * (relaxed atomics) and with no O(n) buffer scan. When the track is idle we
     * compute fresh bins by scanning the buffer directly for full accuracy.
     *
     * @param index Track index
     * @param outBins Output array (caller-allocated, size = numBins)
     * @param numBins Number of bins to generate (1..MAX_WAVEFORM_BINS_CACHE)
     * @return Number of bins written
     */
    int getTrackWaveform(int index, float* outBins, int numBins) {
        if (index < 0 || index >= MAX_TRACKS || !outBins || numBins <= 0) return 0;
        if (!mTracks[index].isActive()) return 0;

        // Decide whether it's safe to read the live buffer.
        // It's NOT safe if: track is being overdubbed, OR track is the active
        // recording target (writeFrame may extend the buffer mid-read).
        const int recTrack = mRecordingTrack.load(std::memory_order_acquire);
        const bool isOverdub = mOverdubbing.load(std::memory_order_acquire);
        const bool busyWriting = (recTrack == index);
        const bool inExport = mExportInProgress.load(std::memory_order_acquire);

        if (busyWriting && (isOverdub || !inExport)) {
            // QW-6: serve the live incremental waveform instead of a stale cache.
            // It's built from peak-per-bin as frames are captured (writeFrame /
            // overdubFrame), so it tracks the take in real time with no O(n) scan
            // and no race (relaxed atomics, writer stores / reader loads).
            return mTracks[index].getLiveWaveform(outBins, numBins);
        }

        // Fresh compute (idle track, UI thread). Read via sampleAt so this works
        // for both the dense and the paged storage backends.
        const TrackBuffer& track = mTracks[index];
        int length = track.getLengthFrames();
        if (length <= 0) return 0;

        const int actualBins = std::min(numBins, MAX_WAVEFORM_BINS_CACHE);
        int framesPerBin = length / actualBins;
        if (framesPerBin <= 0) framesPerBin = 1;

        for (int bin = 0; bin < actualBins; ++bin) {
            float peak = 0.0f;
            int start = bin * framesPerBin;
            int end = std::min(start + framesPerBin, length);
            for (int f = start; f < end; ++f) {
                float absL = std::abs(track.sampleAt(f, 0));
                float absR = std::abs(track.sampleAt(f, 1));
                float m = std::max(absL, absR);
                if (m > peak) peak = m;
            }
            outBins[bin] = peak;
        }
        // Pad caller buffer with zeros if numBins > MAX_WAVEFORM_BINS_CACHE.
        for (int bin = actualBins; bin < numBins; ++bin) outBins[bin] = 0.0f;
        return actualBins;
    }

    float getTrackSpeed(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return 1.0f;
        return mTracks[index].getSpeed();
    }
    void setOverdubGain(float g) { mOverdubGain.store(g, std::memory_order_release); }
    void setOverdubDecay(float d) { mOverdubDecay.store(d, std::memory_order_release); }
    void setEnabled(bool e) { mEnabled.store(e, std::memory_order_release); }
    void setFreeLength(bool f) { mFreeLength.store(f, std::memory_order_release); }

    // ========== Master volume (lock-free) ==========

    void setMasterVolume(float vol) {
        mMasterVolume.store(std::clamp(vol, 0.0f, 1.5f), std::memory_order_release);
    }
    float getMasterVolume() const {
        return mMasterVolume.load(std::memory_order_acquire);
    }

    // ========== Loop Region (lock-free delegates) ==========

    void setTrackLoopRegion(int index, int start, int end) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].setLoopRegion(start, end);
    }
    void resetTrackLoopRegion(int index) {
        if (index >= 0 && index < MAX_TRACKS) mTracks[index].resetLoopRegion();
    }

    /**
     * @brief Onset bounds (first/last audible frame) of a track, for trimming the
     *        leading/trailing silence of a free take. UI/IO thread only.
     * @return (first << 32) | (last & 0xFFFFFFFF); first==last (both 0) if silent
     *         or invalid. `last` is exclusive.
     */
    int64_t findTrackContentBounds(int index, float thresholdRatio) const {
        if (index < 0 || index >= MAX_TRACKS) return 0;
        int first = 0, last = 0;
        mTracks[index].findContentBounds(thresholdRatio, first, last);
        return (static_cast<int64_t>(first) << 32)
             | (static_cast<int64_t>(static_cast<uint32_t>(last)));
    }
    /**
     * @brief Detect onsets in a track for tempo derivation (free auto-loop).
     *        UI/IO thread only. @return number of onsets written.
     */
    int detectTrackOnsets(int index, int* outOnsets, int maxOnsets,
                          int hopFrames, float sensitivity) const {
        if (index < 0 || index >= MAX_TRACKS) return 0;
        return mTracks[index].detectOnsets(outOnsets, maxOnsets, hopFrames, sensitivity);
    }

    /**
     * @brief Bar-snap + seam-bake a free take's loop region (Free-loop auto-sync,
     *        phases A+C). Pads with silence if loopEnd runs past the recording,
     *        bakes the seam wrap-mix when tailFrames>0, and sets the loop region.
     *        UI/IO thread only; no-op while recording into or exporting this track.
     * @return true on success.
     */
    bool finalizeFreeLoop(int index, int loopStart, int loopEnd, int tailFrames) {
        if (index < 0 || index >= MAX_TRACKS) return false;
        if (mRecordingTrack.load(std::memory_order_acquire) == index) return false;
        if (mExportInProgress.load(std::memory_order_acquire)) return false;
        return mTracks[index].finalizeFreeLoop(loopStart, loopEnd, tailFrames);
    }

    int getTrackLoopStart(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return 0;
        return mTracks[index].getLoopStart();
    }
    int getTrackLoopEnd(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return 0;
        return mTracks[index].getLoopEnd();
    }

    /** Trigger a metronome click. RT-safe — can be called from UI or audio thread. */
    void triggerClick(bool isDownbeat) {
        mClick.trigger(isDownbeat);
    }

    // ========== State queries (lock-free) ==========

    /** Progress of the longest active track (0..1). */
    float getProgress() const {
        int longestIdx = -1;
        int longestLen = 0;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isActive()) {
                int len = mTracks[i].getLengthFrames();
                if (len > longestLen) { longestLen = len; longestIdx = i; }
            }
        }
        if (longestIdx >= 0) return mTracks[longestIdx].getProgress();
        return 0.0f;
    }
    float getRecordProgress() const { return mRecordProgress.load(std::memory_order_acquire); }
    /** True if any track is currently playing. */
    bool isPlaying() const {
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isTrackPlaying()) return true;
        }
        return false;
    }
    bool isRecording() const { return mRecordingTrack.load(std::memory_order_acquire) >= 0; }
    int getRecordingTrack() const { return mRecordingTrack.load(std::memory_order_acquire); }
    /** Returns the length of the longest active track (computed, not stored). */
    int getMasterLoopFrames() const {
        int maxLen = 0;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isActive()) {
                int len = mTracks[i].getLengthFrames();
                if (len > maxLen) maxLen = len;
            }
        }
        return maxLen;
    }

    bool isTrackActive(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return false;
        return mTracks[index].isActive();
    }
    float getTrackPeakLevel(int index) const {
        if (index < 0 || index >= MAX_TRACKS) return 0.0f;
        return mTracks[index].getPeakLevel();
    }
    const TrackBuffer& getTrack(int index) const { return mTracks[index]; }

private:
    /**
     * @brief Activate the loop for playback while recording continues into the
     *        tail region. Called when the write head crosses the loop boundary.
     */
    void finalizeLoopStartPlayback(int recTrack) {
        if (recTrack < 0 || recTrack >= MAX_TRACKS) return;
        mTracks[recTrack].finalizeRecording();

        // Phase-lock a sync-armed overdub (armSyncedToLoop) to its reference: set
        // the new track's playhead to the reference's current playhead instead of
        // resetting to 0. Because capture started `latencyFrames` after the
        // reference's loop-zero, the reference is now `latencyFrames` into its
        // loop, so the new track's frame 0 (the user's downbeat) will play exactly
        // when the reference next wraps to 0 — the two loops lock in phase and the
        // round-trip latency is cancelled. -1 (the solo/first-take case) keeps the
        // legacy resetPlayHead(), so that path is unchanged.
        const int syncRef = mSyncRefTrack.load(std::memory_order_acquire);
        if (syncRef >= 0 && syncRef < MAX_TRACKS && syncRef != recTrack
            && mTracks[syncRef].isActive()) {
            mTracks[recTrack].setPlayHeadF(mTracks[syncRef].getPlayHeadF());
        } else {
            mTracks[recTrack].resetPlayHead();
        }
        mSyncRefTrack.store(-1, std::memory_order_release);
        mTracks[recTrack].setPlaying(true);

        // Also start any other active tracks that should be playing.
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isActive() && !mTracks[i].isTrackPlaying()) {
                mTracks[i].setPlaying(true);
            }
        }
    }

    /**
     * @brief Finalize a recording in progress — used by stopRecording() and
     *        free-length mode. Stops capture immediately and starts playback.
     *        Tail region (if any) may be partially captured at this point.
     */
    void finalizeCurrentRecording() {
        int recTrack = mRecordingTrack.load(std::memory_order_acquire);
        if (recTrack < 0 || recTrack >= MAX_TRACKS) return;

        if (!mLoopFinalizedDuringRec.load(std::memory_order_acquire)) {
            finalizeLoopStartPlayback(recTrack);
        }
        mRecordingTrack.store(-1, std::memory_order_release);
        mLoopFinalizedDuringRec.store(false, std::memory_order_release);
    }

    bool hasAnyActiveTracks() const {
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isActive()) return true;
        }
        return false;
    }

    // ========== Export internals ==========

    // The offline exporter reads track buffers + the export-guard atomics below.
    friend class wm::LooperExporter;

    /**
     * @brief RAII guard that disables overdub mutation and clear()/import for
     *        the duration of an export. The audio thread checks
     *        mExportInProgress before performing destructive writes. Kept here
     *        (not in LooperExporter) because it toggles the RT guard atomic.
     */
    class ExportGuard {
    public:
        explicit ExportGuard(AudioLooper& l) : mLooper(l) {
            mLooper.mExportInProgress.store(true, std::memory_order_release);
            mLooper.mCancelExport.store(false, std::memory_order_release);
            mLooper.mExportProgress.store(0.0f, std::memory_order_release);
        }
        ~ExportGuard() {
            mLooper.mExportInProgress.store(false, std::memory_order_release);
        }
        ExportGuard(const ExportGuard&) = delete;
        ExportGuard& operator=(const ExportGuard&) = delete;
    private:
        AudioLooper& mLooper;
    };

    void updateExportProgress(float p) {
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        mExportProgress.store(p, std::memory_order_release);
    }

    // Upper bound on bins produced by the O(n) fresh-compute path in
    // getTrackWaveform (used when the track is idle/finished). The live path
    // (QW-6) has its own resolution in TrackBuffer::kLiveWaveformBins.
    static constexpr int MAX_WAVEFORM_BINS_CACHE = 512;

    size_t getTotalAllocatedBytes() const {
        size_t total = 0;
        for (int i = 0; i < MAX_TRACKS_HW; ++i) {
            total += mTracks[i].allocatedBytes();
        }
        return total;
    }

    // Highest active track index + 1 (0 if none). Used to clamp maxActiveTracks
    // reductions so a lowered limit never stops mixing an already-active track.
    int highestActiveTrackPlusOne() const {
        for (int i = MAX_TRACKS_HW - 1; i >= 0; --i) {
            if (mTracks[i].isActive()) return i + 1;
        }
        return 0;
    }

    TrackBuffer mTracks[MAX_TRACKS_HW];

    // Runtime capabilities (plan §3.2). setCapabilities()/getters are UI thread;
    // mMaxActiveTracks is read from the audio thread each block (atomic).
    std::atomic<size_t> mMemoryBudgetBytes{DEFAULT_MEMORY_BUDGET_BYTES};
    std::atomic<int>    mMaxActiveTracks{DEFAULT_MAX_ACTIVE_TRACKS};
    std::atomic<int>    mMaxFreeSeconds{DEFAULT_MAX_FREE_SECONDS};
    std::atomic<int>    mChunkPoolPrefill{0};

    // Event dispatcher (non-owning). Set once via setEventDispatcher() from
    // the owning AudioEngine before audio callbacks start. Read from the
    // audio thread; never written from RT.
    wm::LooperEventDispatcher* mDispatcher{nullptr};

    // Coalesces + emits push state events (progress/playing/peak/record).
    // Holds the "last emitted" bookkeeping; audio thread only.
    wm::LooperStateEmitter mStateEmitter;
    static_assert(wm::LooperStateEmitter::kMaxTracks >= MAX_TRACKS,
                  "LooperStateEmitter must cover all looper tracks");

    std::atomic<int> mRecordingTrack{-1};
    std::atomic<int> mArmedTrack{-1};
    std::atomic<int64_t> mArmedTriggerFrame{0};
    // Reference track for a sync-armed overdub layer (armSyncedToLoop). When >=0,
    // the just-finalized take is phase-locked to this track's playhead instead of
    // resetting to 0, compensating round-trip latency so the overdub aligns.
    // -1 = no sync (solo/first take) — finalize keeps its legacy resetPlayHead().
    std::atomic<int> mSyncRefTrack{-1};
    std::atomic<bool> mOverdubbing{false};
    std::atomic<bool> mLoopFinalizedDuringRec{false};
    // Default wrap-mix tail window. 500ms gives sustained sounds room to ring
    // across the seam without baking a large chunk of new playing into the loop
    // start. (The old 1750ms was tuned for the removed playback-fade tail.)
    std::atomic<int> mTailMs{500};   // Wrap-mix decay window (ms)
                                    // 750ms covers most natural decays (pads, reverb tails,
                                    // long pianos). NoisyPad can override via setTailMs().

    // Export progress + cancellation. exportInProgress acts as a guard the
    // audio thread should consult before destructive writes (overdub, clear).
    mutable std::atomic<bool>  mExportInProgress{false};
    mutable std::atomic<bool>  mCancelExport{false};
    mutable std::atomic<float> mExportProgress{0.0f};
    // Target export rate (0 = engine rate). Set from the UI before an export.
    std::atomic<int>           mExportSampleRate{0};

    // Telemetry counters (relaxed atomics; observability only, not synchronization).
    mutable std::atomic<int64_t> mFramesDropped{0};
    mutable std::atomic<int64_t> mExportsCompleted{0};
    mutable std::atomic<int64_t> mExportsFailed{0};
    mutable std::atomic<int64_t> mStemsWritten{0};
    mutable std::atomic<int64_t> mArmedTriggered{0};
    std::atomic<int> mRecordFramesRemaining{0};
    std::atomic<int> mRecordCapacityFrames{0};
    std::atomic<int> mMusicalLoopFrames{0};  // Bar-aligned loop length (excludes tail)
    std::atomic<bool> mEnabled{false};
    std::atomic<bool> mFreeLength{false};
    std::atomic<float> mRecordProgress{0.0f};
    std::atomic<float> mOverdubGain{0.8f};
    std::atomic<float> mOverdubDecay{0.0f};

    // Master volume (applied to combined looper output)
    std::atomic<float> mMasterVolume{1.0f};
    std::atomic<float> mMasterVolSmoother{1.0f};

    // Pre-allocated mixing buffer (heap, grows on demand from audio thread).
    // Sized at construction to INITIAL_MIX_CAPACITY_FRAMES, only grows if a callback
    // ever exceeds it (one-shot allocation, then steady-state).
    std::vector<float> mLooperMixBuf;

    // Sample rate (kept in sync with engine via setSampleRate()).
    std::atomic<int> mSampleRate{48000};

    // Metronome / count-in click generator (self-contained, RT-safe).
    wm::MetronomeClick mClick;

};
