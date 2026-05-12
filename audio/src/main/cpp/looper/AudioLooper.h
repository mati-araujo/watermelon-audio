#pragma once

#include "TrackBuffer.h"
#include "WavFile.h"
#include "Limiter.h"
#include "PanLUT.h"
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
    static constexpr int MAX_TRACKS = 8;
    static constexpr size_t MEMORY_BUDGET_BYTES = 48ULL * 1024ULL * 1024ULL;  // 48 MB
    // Click envelope is sample-rate aware; recompute on prepare()/setSampleRate()
    static constexpr float CLICK_DURATION_MS = 10.0f;
    static constexpr float CLICK_FADE_MS = 2.5f;        // ~25% of duration
    // Initial mix buffer capacity; grows on demand if Oboe gives larger callbacks.
    static constexpr int INITIAL_MIX_CAPACITY_FRAMES = 2048;

    AudioLooper() {
        mLooperMixBuf.resize(static_cast<size_t>(INITIAL_MIX_CAPACITY_FRAMES) * 2, 0.0f);
        recomputeClickFrames();
    }
    ~AudioLooper() = default;

    /**
     * @brief Update sample rate. Call from UI/control thread (NOT audio thread).
     *        Recomputes click envelope durations.
     */
    void setSampleRate(int sampleRate) {
        if (sampleRate <= 0) return;
        mSampleRate.store(sampleRate, std::memory_order_release);
        recomputeClickFrames();
    }
    int getSampleRate() const { return mSampleRate.load(std::memory_order_acquire); }

    // ========== Audio thread (RT-safe) ==========

    /**
     * @brief Audio thread tick. The optional `playFrame` parameter is the
     *        Transport's play position at the START of this audio block. It is
     *        used to fire armed recordings exactly at the requested trigger
     *        frame (downbeat-aligned).
     */
    void process(float* audioData, int numFrames, int64_t playFrame = -1) {
        if (!mEnabled.load(std::memory_order_acquire)) return;

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
                // Use the target track's own playhead for overdub position
                int playHead = mTracks[recTrack].getPlayHead();
                float gain = mOverdubGain.load(std::memory_order_relaxed);
                float decay = mOverdubDecay.load(std::memory_order_relaxed);
                for (int i = 0; i < numFrames; ++i) {
                    int pos = (playHead + i) % trackLen;
                    mTracks[recTrack].overdubFrame(
                        pos, audioData[i * 2], audioData[i * 2 + 1], gain, decay);
                }
            } else {
                // Normal recording (with optional tail capture).
                // Capacity here is total buffer length: loopFrames + tailFrames.
                // We split the lifecycle into two checkpoints:
                //   1. Write head crosses loopCapacity  -> finalize loop, start playback
                //      (recording continues into tail region for tailFrames more samples).
                //   2. Write head reaches total capacity -> stop recording entirely.
                int capacity = mRecordCapacityFrames.load(std::memory_order_relaxed);
                int remaining = mRecordFramesRemaining.load(std::memory_order_relaxed);
                bool loopFinalized = mLoopFinalizedDuringRec.load(std::memory_order_relaxed);

                int dropped = 0;
                for (int i = 0; i < numFrames && remaining > 0; ++i) {
                    if (!mTracks[recTrack].writeFrame(audioData[i * 2], audioData[i * 2 + 1])) {
                        ++dropped;
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

                // Checkpoint 2: total capacity reached → stop recording (only in fixed-length mode).
                if (remaining <= 0 && !mFreeLength.load(std::memory_order_relaxed)) {
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
            // RT-unfriendly grow — accept one-time allocation, log it for visibility.
            mLooperMixBuf.resize(needed);
            LOOPER_LOGI("mix buffer grown to %d frames (callback exceeded initial capacity)",
                        numFrames);
        }
        std::memset(mLooperMixBuf.data(), 0, sizeof(float) * needed);

        for (int t = 0; t < MAX_TRACKS; ++t) {
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

        // ---- METRONOME CLICK (pre-count beat indicator — NOT affected by master volume) ----
        if (mClickRemaining.load(std::memory_order_relaxed) > 0) {
            int remaining = mClickRemaining.load(std::memory_order_relaxed);
            int phase = mClickPhase.load(std::memory_order_relaxed);
            float freq = mClickFreq.load(std::memory_order_relaxed);
            float gain = mClickGain.load(std::memory_order_relaxed);
            const float invSr = mInvSampleRate.load(std::memory_order_relaxed);
            const int fadeFrames = mClickFadeFrames.load(std::memory_order_relaxed);
            for (int i = 0; i < numFrames && remaining > 0; ++i) {
                float env = (remaining < fadeFrames && fadeFrames > 0)
                    ? static_cast<float>(remaining) / static_cast<float>(fadeFrames)
                    : 1.0f;
                float sample = std::sin(2.0f * static_cast<float>(M_PI) * freq
                    * static_cast<float>(phase) * invSr) * gain * env;
                audioData[i * 2] += sample;
                audioData[i * 2 + 1] += sample;
                phase++;
                remaining--;
            }
            mClickPhase.store(phase, std::memory_order_relaxed);
            mClickRemaining.store(remaining, std::memory_order_relaxed);
        }
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
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return false;
        if (lengthFrames <= 0) return false;

        // Tail buffer: extra frames captured AFTER loop boundary, mixed with
        // fade-out into the start of the next iteration. Preserves sustain of
        // pads/delays/reverbs across the loop seam. tailMs is global, see
        // setTailMs(). Tail is capped at loopFrames internally by TrackBuffer
        // to keep the math sane for very short loops.
        const int tailMs = mTailMs.load(std::memory_order_acquire);
        const int tailFrames = (tailMs > 0)
            ? (tailMs * sampleRate) / 1000
            : 0;
        const int totalFrames = lengthFrames + tailFrames;

        // Each track can have its own length — no master loop enforcement
        size_t needed = static_cast<size_t>(totalFrames) * 2 * sizeof(float);
        size_t currentUsage = getTotalAllocatedBytes();
        size_t trackCurrent = mTracks[trackIndex].allocatedBytes();
        if (currentUsage - trackCurrent + needed > MEMORY_BUDGET_BYTES) {
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
        int capacity = mTracks[trackIndex].getCapacityFrames();
        if (capacity <= 0) return;
        mRecordCapacityFrames.store(capacity, std::memory_order_release);
        mRecordFramesRemaining.store(capacity, std::memory_order_release);
        // Musical loop length (excludes tail) — used by recording progress so the
        // bar fills in lockstep with the bar boundary, not the post-tail boundary.
        mMusicalLoopFrames.store(mTracks[trackIndex].getLoopCapacityFrames(),
                                 std::memory_order_release);
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
        mArmedTriggerFrame.store(triggerFrame, std::memory_order_release);
        mArmedTrack.store(trackIndex, std::memory_order_release);
        mEnabled.store(true, std::memory_order_release);
    }

    /** Cancel any pending armed recording (does not affect a recording in progress). */
    void cancelArm() {
        mArmedTrack.store(-1, std::memory_order_release);
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

    void clearAll() {
        mRecordingTrack.store(-1, std::memory_order_release);
        mArmedTrack.store(-1, std::memory_order_release);
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

    /**
     * @brief Options for exportMix / exportStems.
     *
     * Most fields have safe defaults — backward-compat callers can use the
     * single-arg `exportMix(path)` overload below.
     */
    struct ExportOptions {
        wav::BitDepth bitDepth = wav::BitDepth::PCM_16;
        int repeatLoops = 1;       // export N iterations of the loop length
        int countInFrames = 0;     // leading silence (e.g. = N beats * framesPerBeat)
        bool applyLimiter = true;  // true-peak limiter instead of tanh soft-clip
        wav::WavMetadata metadata; // BPM, project name, etc. — embedded in WAV
    };

    /**
     * @brief Export mix of all active tracks to a WAV file.
     *
     * Uses an "export guard" (mExportInProgress) that the audio thread checks
     * before running overdub or finalize, so the buffer contents we read here
     * cannot be mutated mid-snapshot. Playback continues normally during export.
     * clear()/importTrack() callers are expected to also respect the guard
     * (this is enforced by JNI serialization on the UI/IO thread).
     *
     * @param filePath Output file path.
     * @param opts     Export options (bit depth, repeat, count-in, limiter, metadata).
     * @return true if successful.
     */
    bool exportMix(const char* filePath, const ExportOptions& opts) {
        return exportMixInternal(filePath, opts);
    }

    /** Backward-compat: defaults (16-bit, 1 loop, no count-in, limiter on). */
    bool exportMix(const char* filePath) {
        return exportMixInternal(filePath, ExportOptions{});
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
    int exportStems(const char* directory, const ExportOptions& opts) {
        if (!directory) return -1;
        const ExportGuard guard(*this);

        const ExportSnapshot snap = takeSnapshot();
        if (snap.frames <= 0) return -1;

        const int totalFrames = snap.frames * std::max(1, opts.repeatLoops)
                              + std::max(0, opts.countInFrames);
        const int sr = (snap.sampleRate > 0) ? snap.sampleRate : 48000;

        std::string base = directory;
        if (!base.empty() && base.back() != '/' && base.back() != '\\') base.push_back('/');

        wm::OfflineLimiter limiter;
        if (opts.applyLimiter) limiter.prepare(sr);

        std::vector<float> stem(static_cast<size_t>(totalFrames) * 2, 0.0f);
        int written = 0;
        for (int t = 0; t < MAX_TRACKS; ++t) {
            if (mCancelExport.load(std::memory_order_acquire)) return -1;
            const auto& ts = snap.tracks[t];
            if (!ts.active || ts.muted || ts.length <= 0) continue;

            std::fill(stem.begin(), stem.end(), 0.0f);
            mixTrackInto(stem.data(), totalFrames, ts, opts.countInFrames);
            if (opts.applyLimiter) limiter.processStereo(stem.data(), totalFrames);

            const std::string path = base + "track_" + std::to_string(t) + ".wav";
            if (!wav::writeWav(path.c_str(), stem.data(), totalFrames,
                               sr, opts.bitDepth, opts.metadata)) {
                LOOPER_LOGE("exportStems: failed to write %s", path.c_str());
                continue;
            }
            ++written;
            mStemsWritten.fetch_add(1, std::memory_order_relaxed);
            updateExportProgress(static_cast<float>(written) /
                                 static_cast<float>(MAX_TRACKS));
        }
        mExportProgress.store(1.0f, std::memory_order_release);
        if (written > 0) mExportsCompleted.fetch_add(1, std::memory_order_relaxed);
        else             mExportsFailed.fetch_add(1, std::memory_order_relaxed);
        return written;
    }

    /**
     * @brief Export a single track to a WAV file.
     * @param trackIndex Track to export (0-7)
     * @param filePath   Output file path
     * @param opts       Export options (defaults: 16-bit, no metadata, no limiter
     *                   to preserve original dynamics for DAW import).
     * @return true if successful
     */
    bool exportTrack(int trackIndex, const char* filePath,
                     const ExportOptions& opts) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return false;
        if (!mTracks[trackIndex].isActive()) return false;

        const ExportGuard guard(*this);
        const float* data = mTracks[trackIndex].data();
        int len = mTracks[trackIndex].getLengthFrames();
        int sr = mTracks[trackIndex].getSampleRate();

        // If a custom loop region is defined, export only the region
        int regionStart = mTracks[trackIndex].getLoopStart();
        int regionEnd = mTracks[trackIndex].getLoopEnd();
        const float* writeData = data;
        int writeLen = len;
        if (regionStart > 0 || regionEnd < len) {
            int regionLen = regionEnd - regionStart;
            if (regionLen > 0) {
                writeData = data + (static_cast<size_t>(regionStart) * 2);
                writeLen = regionLen;
            }
        }
        return wav::writeWav(filePath, writeData, writeLen, sr,
                             opts.bitDepth, opts.metadata);
    }

    /** Backward-compat overload. */
    bool exportTrack(int trackIndex, const char* filePath) {
        return exportTrack(trackIndex, filePath, ExportOptions{});
    }

    // ========== Export progress / cancel ==========

    /** [0..1] progress of last/in-flight export. Lock-free. */
    float getExportProgress() const {
        return mExportProgress.load(std::memory_order_acquire);
    }

    /** Set the cancel flag. exportMix/exportStems will bail at the next iteration. */
    void cancelExport() {
        mCancelExport.store(true, std::memory_order_release);
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
    bool importTrack(int trackIndex, const char* filePath, int sampleRate) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return false;

        LOOPER_LOGD("importTrack: reading %s", filePath);
        wav::WavData wavData = wav::readWav(filePath);
        if (wavData.numFrames <= 0) {
            LOOPER_LOGE("importTrack FAILED: readWav returned 0 frames (unsupported format or corrupt file)");
            return false;
        }
        LOOPER_LOGD("importTrack: %d frames, %dHz, %d ch", wavData.numFrames, wavData.sampleRate, wavData.numChannels);

        // Resample if source sample rate differs from target (e.g., 44100 → 48000)
        bool needsResample = (wavData.sampleRate > 0 && wavData.sampleRate != sampleRate);
        int outputFrames = wavData.numFrames;
        std::vector<float> resampledBuffer;

        if (needsResample) {
            double ratio = static_cast<double>(sampleRate) / static_cast<double>(wavData.sampleRate);
            outputFrames = static_cast<int>(std::ceil(wavData.numFrames * ratio));
            LOOPER_LOGD("Resampling %dHz -> %dHz (ratio=%.4f, %d -> %d frames)",
                        wavData.sampleRate, sampleRate, ratio, wavData.numFrames, outputFrames);

            resampledBuffer.resize(static_cast<size_t>(outputFrames) * 2);
            // Catmull-Rom cubic resample. For boundary frames the neighbours
            // clamp to [0, numFrames-1] (no wrap — sources are not loops).
            const int srcLast = wavData.numFrames - 1;
            for (int i = 0; i < outputFrames; ++i) {
                const double srcPos = i / ratio;
                const int s1 = std::min(static_cast<int>(srcPos), srcLast);
                const int s0 = (s1 > 0) ? s1 - 1 : 0;
                const int s2 = std::min(s1 + 1, srcLast);
                const int s3 = std::min(s1 + 2, srcLast);
                const float t = static_cast<float>(srcPos - s1);
                const float t2 = t * t;
                const float t3 = t2 * t;
                for (int ch = 0; ch < 2; ++ch) {
                    const float p0 = wavData.buffer[s0 * 2 + ch];
                    const float p1 = wavData.buffer[s1 * 2 + ch];
                    const float p2 = wavData.buffer[s2 * 2 + ch];
                    const float p3 = wavData.buffer[s3 * 2 + ch];
                    resampledBuffer[i * 2 + ch] = 0.5f * ((2.0f * p1)
                        + (-p0 + p2) * t
                        + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                        + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
                }
            }
        }

        const float* srcBuffer = needsResample ? resampledBuffer.data() : wavData.buffer.data();

        // Check memory budget
        size_t needed = static_cast<size_t>(outputFrames) * 2 * sizeof(float);
        size_t currentUsage = getTotalAllocatedBytes();
        size_t trackCurrent = mTracks[trackIndex].allocatedBytes();
        if (currentUsage - trackCurrent + needed > MEMORY_BUDGET_BYTES) {
            LOOPER_LOGE("importTrack FAILED: memory budget exceeded (need %zu, budget %zu, used %zu)",
                        needed, MEMORY_BUDGET_BYTES, currentUsage - trackCurrent);
            return false;
        }

        // Stop playback on this track before clearing to avoid race with audio thread.
        mTracks[trackIndex].setMuted(true);
        mTracks[trackIndex].setPlaying(false);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Clear existing content
        mTracks[trackIndex].clear();

        // Allocate and fill with (possibly resampled) data
        size_t allocated = mTracks[trackIndex].allocate(outputFrames, sampleRate);
        if (allocated == 0) return false;

        for (int i = 0; i < outputFrames; ++i) {
            mTracks[trackIndex].writeFrame(srcBuffer[i * 2], srcBuffer[i * 2 + 1]);
        }
        mTracks[trackIndex].finalizeRecording();
        mTracks[trackIndex].setMuted(false);
        mEnabled.store(true, std::memory_order_release);
        return true;
    }

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
     * @brief Get a waveform summary (peak amplitudes) for visualization.
     *
     * Race protection: if the track is currently being overdubbed (audio thread
     * mutating the buffer), we return the last cached snapshot instead of
     * reading the live buffer. Overdub-vs-read had no synchronization in the
     * previous implementation; the cache keeps visualization smooth without
     * pausing the audio thread or copying the entire buffer per poll.
     *
     * The first call (or any call when the track is not being overdubbed)
     * computes fresh bins by reading the buffer directly. Subsequent calls
     * during overdub return whatever was cached at the last fresh read.
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

        if (busyWriting && isOverdub) {
            // Serve cache — bail to avoid race with overdub mutations.
            return readWaveformCache(index, outBins, numBins);
        }
        if (busyWriting && !inExport) {
            // Active fresh recording — buffer is being appended to. Reading
            // up to current length is technically safe (writes go ahead of
            // length), but we serve cache for consistency.
            return readWaveformCache(index, outBins, numBins);
        }

        // Fresh compute + update cache.
        const float* data = mTracks[index].data();
        int length = mTracks[index].getLengthFrames();
        if (length <= 0) return 0;

        const int actualBins = std::min(numBins, MAX_WAVEFORM_BINS_CACHE);
        int framesPerBin = length / actualBins;
        if (framesPerBin <= 0) framesPerBin = 1;

        auto& cache = mWaveformCache[index];
        for (int bin = 0; bin < actualBins; ++bin) {
            float peak = 0.0f;
            int start = bin * framesPerBin;
            int end = std::min(start + framesPerBin, length);
            for (int f = start; f < end; ++f) {
                float absL = std::abs(data[f * 2]);
                float absR = std::abs(data[f * 2 + 1]);
                float m = std::max(absL, absR);
                if (m > peak) peak = m;
            }
            outBins[bin] = peak;
            cache.bins[bin].store(peak, std::memory_order_relaxed);
        }
        cache.binCount.store(actualBins, std::memory_order_release);
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
        mClickFreq.store(isDownbeat ? 1200.0f : 900.0f, std::memory_order_relaxed);
        mClickGain.store(isDownbeat ? 0.35f : 0.25f, std::memory_order_relaxed);
        mClickPhase.store(0, std::memory_order_relaxed);
        mClickRemaining.store(mClickDurationFrames.load(std::memory_order_relaxed),
                              std::memory_order_release);
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
        mTracks[recTrack].resetPlayHead();
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

    struct TrackSnapshot {
        bool active = false;
        bool muted = false;
        int  length = 0;
        int  loopStart = 0;
        int  loopEnd = 0;
        float volume = 1.0f;
        float pan = 0.0f;
        const float* data = nullptr;  // Points into TrackBuffer; valid only while ExportGuard alive.
    };

    struct ExportSnapshot {
        TrackSnapshot tracks[MAX_TRACKS];
        int frames = 0;        // longest active track length (no count-in / repeat applied)
        int sampleRate = 48000;
    };

    /**
     * @brief RAII guard that disables overdub mutation and clear()/import for
     *        the duration of an export. The audio thread checks
     *        mExportInProgress before performing destructive writes.
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

    ExportSnapshot takeSnapshot() const {
        ExportSnapshot s;
        s.sampleRate = mSampleRate.load(std::memory_order_acquire);
        for (int t = 0; t < MAX_TRACKS; ++t) {
            const auto& track = mTracks[t];
            auto& ts = s.tracks[t];
            ts.active = track.isActive();
            if (!ts.active) continue;
            ts.muted = track.isMuted();
            ts.length = track.getLengthFrames();
            ts.loopStart = track.getLoopStart();
            ts.loopEnd = track.getLoopEnd();
            ts.volume = track.getVolume();
            ts.pan = track.getPan();
            ts.data = track.data();
            if (ts.length > s.frames) s.frames = ts.length;
            if (s.sampleRate <= 0) s.sampleRate = track.getSampleRate();
        }
        return s;
    }

    /**
     * @brief Mix a single snapshotted track into `output` for `outputFrames`
     *        starting at frame `countInFrames` (offset for leading silence).
     *        Shorter tracks are looped within their loop region.
     */
    void mixTrackInto(float* output, int outputFrames,
                      const TrackSnapshot& ts, int countInFrames) const {
        if (!ts.active || ts.muted || !ts.data || ts.length <= 0) return;
        const int loopStart = ts.loopStart;
        const int loopEnd = (ts.loopEnd > 0) ? ts.loopEnd : ts.length;
        const int loopLen = std::max(1, loopEnd - loopStart);

        const auto pp = wm::EqualPowerPanLUT::instance().lookup(ts.pan);
        const float gainL = ts.volume * pp.l;
        const float gainR = ts.volume * pp.r;

        for (int i = countInFrames; i < outputFrames; ++i) {
            const int t = i - countInFrames;
            const int pos = loopStart + (t % loopLen);
            output[i * 2]     += ts.data[pos * 2]     * gainL;
            output[i * 2 + 1] += ts.data[pos * 2 + 1] * gainR;
        }
    }

    bool exportMixInternal(const char* filePath, const ExportOptions& opts) {
        if (!filePath) return false;
        ExportGuard guard(*this);

        const ExportSnapshot snap = takeSnapshot();
        if (snap.frames <= 0) return false;

        const int repeats = std::max(1, opts.repeatLoops);
        const int countIn = std::max(0, opts.countInFrames);
        const int totalFrames = snap.frames * repeats + countIn;
        const int sr = (snap.sampleRate > 0) ? snap.sampleRate : 48000;

        std::vector<float> mixBuffer(static_cast<size_t>(totalFrames) * 2, 0.0f);

        // Mix each track. Progress is reported per active track.
        int activeCount = 0;
        for (int t = 0; t < MAX_TRACKS; ++t) {
            if (snap.tracks[t].active && !snap.tracks[t].muted) ++activeCount;
        }
        if (activeCount == 0) {
            // All tracks muted/inactive — write silence (still useful for count-in tests).
            return wav::writeWav(filePath, mixBuffer.data(), totalFrames,
                                 sr, opts.bitDepth, opts.metadata);
        }

        int processed = 0;
        for (int t = 0; t < MAX_TRACKS; ++t) {
            if (mCancelExport.load(std::memory_order_acquire)) return false;
            const auto& ts = snap.tracks[t];
            if (!ts.active || ts.muted) continue;
            mixTrackInto(mixBuffer.data(), totalFrames, ts, countIn);
            ++processed;
            // Reserve [0..0.85] for mixing, [0.85..1.0] for limiter+IO.
            updateExportProgress(0.85f * static_cast<float>(processed) /
                                 static_cast<float>(activeCount));
        }

        if (mCancelExport.load(std::memory_order_acquire)) return false;

        if (opts.applyLimiter) {
            wm::OfflineLimiter limiter;
            limiter.prepare(sr);
            limiter.processStereo(mixBuffer.data(), totalFrames);
        }
        updateExportProgress(0.95f);

        const bool ok = wav::writeWav(filePath, mixBuffer.data(), totalFrames,
                                      sr, opts.bitDepth, opts.metadata);
        mExportProgress.store(1.0f, std::memory_order_release);
        if (ok) mExportsCompleted.fetch_add(1, std::memory_order_relaxed);
        else    mExportsFailed.fetch_add(1, std::memory_order_relaxed);
        return ok;
    }

    void updateExportProgress(float p) {
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        mExportProgress.store(p, std::memory_order_release);
    }

    int readWaveformCache(int index, float* outBins, int numBins) const {
        const auto& cache = mWaveformCache[index];
        const int cached = cache.binCount.load(std::memory_order_acquire);
        const int n = std::min(numBins, cached);
        for (int i = 0; i < n; ++i) {
            outBins[i] = cache.bins[i].load(std::memory_order_relaxed);
        }
        for (int i = n; i < numBins; ++i) outBins[i] = 0.0f;
        return n;
    }

    static constexpr int MAX_WAVEFORM_BINS_CACHE = 512;
    struct WaveformCache {
        std::atomic<int> binCount{0};
        std::atomic<float> bins[MAX_WAVEFORM_BINS_CACHE]{};
    };

    size_t getTotalAllocatedBytes() const {
        size_t total = 0;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            total += mTracks[i].allocatedBytes();
        }
        return total;
    }

    TrackBuffer mTracks[MAX_TRACKS];

    std::atomic<int> mRecordingTrack{-1};
    std::atomic<int> mArmedTrack{-1};
    std::atomic<int64_t> mArmedTriggerFrame{0};
    std::atomic<bool> mOverdubbing{false};
    std::atomic<bool> mLoopFinalizedDuringRec{false};
    std::atomic<int> mTailMs{750};  // Default tail capture (preserves sustain at loop seam)
                                    // 750ms covers most natural decays (pads, reverb tails,
                                    // long pianos). NoisyPad can override via setTailMs().

    // Export progress + cancellation. exportInProgress acts as a guard the
    // audio thread should consult before destructive writes (overdub, clear).
    mutable std::atomic<bool>  mExportInProgress{false};
    mutable std::atomic<bool>  mCancelExport{false};
    mutable std::atomic<float> mExportProgress{0.0f};

    // Telemetry counters (relaxed atomics; observability only, not synchronization).
    WaveformCache mWaveformCache[MAX_TRACKS]{};

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
    std::atomic<float> mInvSampleRate{1.0f / 48000.0f};
    std::atomic<int> mClickDurationFrames{480};
    std::atomic<int> mClickFadeFrames{120};

    // Metronome click state (lock-free)
    std::atomic<int> mClickRemaining{0};
    std::atomic<int> mClickPhase{0};
    std::atomic<float> mClickFreq{1000.0f};
    std::atomic<float> mClickGain{0.3f};

    void recomputeClickFrames() {
        const int sr = mSampleRate.load(std::memory_order_relaxed);
        if (sr <= 0) return;
        mInvSampleRate.store(1.0f / static_cast<float>(sr), std::memory_order_relaxed);
        mClickDurationFrames.store(
            static_cast<int>(CLICK_DURATION_MS * 0.001f * static_cast<float>(sr)),
            std::memory_order_relaxed);
        mClickFadeFrames.store(
            static_cast<int>(CLICK_FADE_MS * 0.001f * static_cast<float>(sr)),
            std::memory_order_relaxed);
    }
};
