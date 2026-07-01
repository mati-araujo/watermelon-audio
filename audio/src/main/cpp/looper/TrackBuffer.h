#pragma once

#include "PanLUT.h"
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstring>
#include <cmath>

/**
 * @class TrackBuffer
 * @brief RT-safe audio buffer for a single looper track
 *
 * Stores stereo interleaved audio data captured from the output.
 * Supports recording, looped playback with crossfade, overdub with soft-clip,
 * and single-level undo.
 *
 * Thread Safety:
 * - allocate(), clear(), saveUndoSnapshot(), restoreUndo() → UI thread only
 * - writeFrame(), mixInto(), overdubFrame() → audio thread only (RT-safe)
 * - Atomic fields for cross-thread state queries (mActive, mMuted, mVolume, etc.)
 *
 * Memory:
 * - Buffer allocated lazily via allocate() (not in constructor)
 * - Undo buffer allocated lazily on first overdub
 */
class TrackBuffer {
public:
    static constexpr int CROSSFADE_FRAMES = 128;  // ~2.7ms @ 48kHz — minimum/fallback for very short loops
    static constexpr float DEFAULT_SEAM_CROSSFADE_MS = 50.0f;  // long, musical crossfade at the loop seam

    TrackBuffer() = default;
    ~TrackBuffer() = default;

    // Non-copyable (large buffers)
    TrackBuffer(const TrackBuffer&) = delete;
    TrackBuffer& operator=(const TrackBuffer&) = delete;

    // ========== Lifecycle (UI thread) ==========

    /**
     * @brief Allocate buffer for recording. Call from UI thread before arming.
     * @param loopFrames Musical loop length in stereo frames.
     * @param sampleRate Sample rate at time of allocation.
     * @param tailFrames Additional frames captured AFTER the loop boundary, mixed
     *                   into the start of the next iteration with linear fade-out.
     *                   This preserves the natural decay of sustained sounds at
     *                   the loop seam. Default 0 = legacy behavior (no tail).
     * @return Allocated size in bytes, or 0 on failure.
     */
    size_t allocate(int loopFrames, int sampleRate, int tailFrames = 0) {
        if (loopFrames <= 0) return 0;
        if (tailFrames < 0) tailFrames = 0;
        // Cap tail at loopFrames to avoid pathological cases with very short loops.
        if (tailFrames > loopFrames) tailFrames = loopFrames;
        try {
            // Buffer is exactly the loop body. The wrap-mix tail overdubs INTO the
            // loop start at record time, so no separate tail region is allocated;
            // tailFrames is kept only as the wrap-mix decay window length.
            mBuffer.resize(static_cast<size_t>(loopFrames) * 2, 0.0f);
        } catch (...) {
            return 0;
        }
        mCapacityFrames = loopFrames;
        mLoopCapacityFrames = loopFrames;
        mTailFrames.store(tailFrames, std::memory_order_release);
        mSampleRate = sampleRate;
        // Compute seam crossfade in frames from the configured ms default. Equal-power
        // crossfade smooths the loop seam for sustained material (pads, reverb tails).
        const int seamXf = static_cast<int>(DEFAULT_SEAM_CROSSFADE_MS * 0.001f
                                          * static_cast<float>(sampleRate));
        mSeamCrossfadeFrames.store(std::max(CROSSFADE_FRAMES, seamXf),
                                   std::memory_order_release);
        mWriteHead.store(0, std::memory_order_release);
        mLengthFrames.store(0, std::memory_order_release);
        mActive.store(false, std::memory_order_release);
        mPeakLevel.store(0.0f, std::memory_order_release);
        return mBuffer.size() * sizeof(float);
    }

    /** Returns the loop length capacity (musical loop, NOT including tail). */
    int getLoopCapacityFrames() const { return mLoopCapacityFrames; }
    /** Returns configured tail frames. */
    int getTailFrames() const { return mTailFrames.load(std::memory_order_acquire); }

    /**
     * @brief Release all memory.
     */
    void clear() {
        // Step 1: Stop audio thread from reading this track.
        // mPlaying=false prevents mixInto() from processing.
        // mActive=false is a secondary guard.
        // mLengthFrames=0 causes early return even if active check races.
        mPlaying.store(false, std::memory_order_release);
        mActive.store(false, std::memory_order_release);
        mLengthFrames.store(0, std::memory_order_release);

        // Step 2: Full fence ensures all stores above are visible to audio thread
        // before we deallocate the heap buffer.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Step 2b: Wait out any in-flight mixInto() for this track. The early-return
        // guards above only protect mixInto calls that START after this point; a
        // call already mid-render holds a stale buffer pointer, so we must let it
        // finish before freeing (else use-after-free in the interp read).
        waitForRenderIdle();

        // Step 3: Now safe to deallocate — no mixInto is reading mBuffer.
        mPlayHead.store(0, std::memory_order_relaxed);
        mPlayHeadF.store(0.0f, std::memory_order_relaxed);
        mProgress.store(0.0f, std::memory_order_relaxed);
        mSpeed.store(1.0f, std::memory_order_relaxed);
        mWriteHead.store(0, std::memory_order_relaxed);
        mPeakLevel.store(0.0f, std::memory_order_relaxed);
        mHasUndo.store(false, std::memory_order_relaxed);
        mMuteGainSmoother.store(1.0f, std::memory_order_relaxed);
        mVolumeSmoother.store(1.0f, std::memory_order_relaxed);
        mPanSmoother.store(0.0f, std::memory_order_relaxed);
        mLoopStart.store(0, std::memory_order_relaxed);
        mLoopEnd.store(0, std::memory_order_relaxed);
        // Free the heap buffers outright (swap-with-empty). Previously we retained
        // capacity() to skip re-allocation on re-record, but that made the memory
        // budget accounting (allocatedBytes() counts capacity()) hold on to large
        // buffers — fatal for free-length tracks which pre-size to MAX_FREE seconds.
        // Releasing here keeps getTotalAllocatedBytes() honest so prepareTrack()
        // never falsely fails the 48 MB budget after a clear/record cycle.
        std::vector<float>().swap(mBuffer);
        std::vector<float>().swap(mUndoBuffer);
        // Note: mCapacityFrames is intentionally NOT reset — it tracks the LOGICAL track
        // capacity (set in allocate()), which becomes meaningless once length=0. Reset
        // here prevents writeFrame() from accepting frames before next allocate().
        mCapacityFrames = 0;
        mLoopCapacityFrames = 0;
        mTailFrames.store(0, std::memory_order_relaxed);
        mTailEnabled.store(true, std::memory_order_relaxed);  // back to Sustained default
    }

    /**
     * @brief Get allocated size in bytes (for memory budget tracking).
     */
    size_t allocatedBytes() const {
        return mBuffer.capacity() * sizeof(float) + mUndoBuffer.capacity() * sizeof(float);
    }

    // ========== Recording (audio thread) ==========

    /**
     * @brief Write one stereo frame during recording. RT-safe.
     * @return true if frame written, false if buffer full
     */
    bool writeFrame(float left, float right) {
        int pos = mWriteHead.load(std::memory_order_relaxed);
        if (pos >= mCapacityFrames) return false;

        mBuffer[static_cast<size_t>(pos) * 2]     = left;
        mBuffer[static_cast<size_t>(pos) * 2 + 1] = right;

        pos++;
        mWriteHead.store(pos, std::memory_order_relaxed);
        // Loop length saturates at loopCapacity. The tail region (pos > loopCap)
        // is captured into the buffer but not exposed to playback as loop length —
        // mixInto() reads it via the tail-mix path with fade-out instead.
        const int lenForLoop = (mLoopCapacityFrames > 0 && pos > mLoopCapacityFrames)
                               ? mLoopCapacityFrames
                               : pos;
        mLengthFrames.store(lenForLoop, std::memory_order_release);
        return true;
    }

    /** True iff write head has reached or passed the musical loop boundary. */
    bool hasReachedLoopEnd() const {
        if (mLoopCapacityFrames <= 0) return false;
        return mWriteHead.load(std::memory_order_acquire) >= mLoopCapacityFrames;
    }

    /**
     * @brief Finalize recording. Call from audio thread when done.
     */
    void finalizeRecording() {
        mActive.store(true, std::memory_order_release);
    }

    /**
     * @brief Shrink the heap buffer down to the actually-recorded length,
     *        releasing the unused tail of a pre-sized buffer. UI/IO thread ONLY.
     *
     * Free-length tracks are allocated to MAX_FREE seconds (~60s ≈ 23 MB) but
     * the take usually ends far earlier. Without trimming, every free track
     * holds its full pre-size against the 48 MB budget, so the 2nd free take
     * fails prepareTrack(). This reallocs to exactly [0, length) frames.
     *
     * RT-safety: mirrors importTrack()'s pattern — pause playback, full fence,
     * then realloc. The audio thread skips a paused/short track in mixInto().
     * The (fractional) playhead is preserved; playback resumes seamlessly.
     *
     * @return true if the buffer was trimmed, false if nothing to do / failure.
     */
    bool trimToLength() {
        const int len = mLengthFrames.load(std::memory_order_acquire);
        if (len <= 0) return false;
        const int curFrames = static_cast<int>(mBuffer.size() / 2);
        if (curFrames <= len) return false;  // nothing to trim

        const bool wasPlaying = mPlaying.load(std::memory_order_acquire);
        mPlaying.store(false, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        waitForRenderIdle();  // no mixInto may be reading mBuffer while we realloc

        try {
            std::vector<float> trimmed(mBuffer.begin(),
                                       mBuffer.begin() + static_cast<size_t>(len) * 2);
            mBuffer.swap(trimmed);
        } catch (...) {
            if (wasPlaying) mPlaying.store(true, std::memory_order_release);
            return false;
        }

        mCapacityFrames = len;
        mLoopCapacityFrames = std::min(mLoopCapacityFrames, len);
        // The dedicated tail region (if any) lived past the loop boundary; after a
        // free-take trim it's gone, so disable tail mixing to avoid reading OOB.
        mTailFrames.store(0, std::memory_order_release);
        mWriteHead.store(len, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (wasPlaying) mPlaying.store(true, std::memory_order_release);
        return true;
    }

    // ========== Playback (audio thread) ==========

    /**
     * @brief Mix this track into an output buffer using the track's own playhead.
     *        Applies crossfade at loop wrap-around point. RT-safe.
     *        Only produces output if both mActive and mPlaying are true.
     * @param output Stereo interleaved output buffer to ADD into (not overwrite)
     * @param numFrames Number of frames to process
     */
    void mixInto(float* output, int numFrames) {
        // Marks this track as "rendering" for the whole block so clear()/trim/
        // finalizeFreeLoop() can't free mBuffer mid-read. Cleared on every return.
        RenderScope renderScope(mRendering);

        if (!mActive.load(std::memory_order_acquire)) return;
        if (!mPlaying.load(std::memory_order_acquire)) return;

        int length = mLengthFrames.load(std::memory_order_acquire);
        if (length <= 0) return;

        // Load loop region boundaries
        int loopStart = mLoopStart.load(std::memory_order_acquire);
        int loopEnd = mLoopEnd.load(std::memory_order_acquire);
        if (loopEnd <= 0) loopEnd = length;  // 0 = use full buffer
        int loopLen = loopEnd - loopStart;
        if (loopLen <= 0) return;

        // Adaptive seam crossfade: prefer the configured long window (default 50ms);
        // shrink for short loop regions so the crossfade never exceeds half the loop.
        int crossfadeFrames = mSeamCrossfadeFrames.load(std::memory_order_relaxed);
        if (loopLen < crossfadeFrames * 2) {
            crossfadeFrames = loopLen / 2;
        }

        float playHeadF = mPlayHeadF.load(std::memory_order_relaxed);
        float speed = mSpeed.load(std::memory_order_relaxed);

        // Mute target: 0.0 when muted, 1.0 when unmuted
        // Smoothing gives ~5ms fade at 48kHz (240 frames)
        float muteTarget = mMuted.load(std::memory_order_acquire) ? 0.0f : 1.0f;
        float muteGain = mMuteGainSmoother.load(std::memory_order_relaxed);

        // Volume smoothing
        float vol = mVolumeSmoother.load(std::memory_order_relaxed);
        float targetVol = mVolume.load(std::memory_order_acquire);

        // Pan: equal-power panning with smoothing (constant power)
        float panSmooth = mPanSmoother.load(std::memory_order_relaxed);
        float targetPan = mPan.load(std::memory_order_acquire);

        constexpr float kSmoothCoeff = 0.995f;  // ~5ms at 48kHz
        float peakL = 0.0f;
        float peakR = 0.0f;

        // NOTE: the loop seam's tail/decay is no longer mixed at playback. The
        // ringing continuation past the boundary is now overdubbed (wrap-mixed)
        // into the loop START at RECORD time (see AudioLooper::process), so it is
        // already baked into the buffer here. The equal-power crossfade below still
        // smooths the cut itself.
        const auto& panLut = wm::EqualPowerPanLUT::instance();
        for (int i = 0; i < numFrames; ++i) {
            // Smooth volume, mute gain, and pan
            vol = kSmoothCoeff * vol + (1.0f - kSmoothCoeff) * targetVol;
            muteGain = kSmoothCoeff * muteGain + (1.0f - kSmoothCoeff) * muteTarget;
            panSmooth = kSmoothCoeff * panSmooth + (1.0f - kSmoothCoeff) * targetPan;

            // Equal-power pan via LUT (replaces cos/sin per sample).
            const auto pp = panLut.lookup(panSmooth);
            const float panL = pp.l;
            const float panR = pp.r;

            // Fractional position WITHIN the loop region
            float regionPos = std::fmod(playHeadF + static_cast<float>(i) * speed,
                                        static_cast<float>(loopLen));
            if (regionPos < 0.0f) regionPos += static_cast<float>(loopLen);

            // Absolute position in buffer = loopStart + regionPos
            float fPos = static_cast<float>(loopStart) + regionPos;

            // Catmull-Rom cubic interpolation (4-tap). Significantly reduces
            // aliasing artifacts vs linear interpolation when speed != 1.0,
            // while staying RT-safe (no transcendentals, no allocs). Neighbours
            // wrap inside the loop region so playback at non-integer speeds
            // remains seamless across the loop boundary.
            int pos1 = static_cast<int>(fPos);
            int pos0 = pos1 - 1; if (pos0 < loopStart) pos0 = loopEnd - 1;
            int pos2 = pos1 + 1; if (pos2 >= loopEnd) pos2 = loopStart;
            int pos3 = pos2 + 1; if (pos3 >= loopEnd) pos3 = loopStart;
            float t = fPos - static_cast<float>(pos1);

            // Catmull-Rom basis: y(t) = 0.5 * ((2*p1) + (-p0+p2)*t +
            //   (2*p0 - 5*p1 + 4*p2 - p3)*t^2 + (-p0 + 3*p1 - 3*p2 + p3)*t^3)
            const float t2 = t * t;
            const float t3 = t2 * t;
            auto interp = [&](int channel) -> float {
                const float p0 = mBuffer[static_cast<size_t>(pos0) * 2 + channel];
                const float p1 = mBuffer[static_cast<size_t>(pos1) * 2 + channel];
                const float p2 = mBuffer[static_cast<size_t>(pos2) * 2 + channel];
                const float p3 = mBuffer[static_cast<size_t>(pos3) * 2 + channel];
                return 0.5f * ((2.0f * p1)
                             + (-p0 + p2) * t
                             + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                             + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
            };
            float sampleL = interp(0);
            float sampleR = interp(1);

            // Equal-power crossfade at loop region boundary. pos1 is the floor of
            // the fractional position (the "current" integer sample). When we are
            // within `crossfadeFrames` of loopEnd, blend the current sample with
            // the matching sample at the START of the loop using a sqrt curve so
            // total energy stays constant across the seam — masks the cut for
            // sustained material (pads, reverb tails) at the cost of doubling the
            // material in the crossfade window.
            int distToEnd = loopEnd - pos1;
            if (distToEnd <= crossfadeFrames && crossfadeFrames > 0) {
                float fade  = static_cast<float>(distToEnd) / static_cast<float>(crossfadeFrames);
                int wrapPos = loopStart + (crossfadeFrames - distToEnd);
                if (wrapPos < loopEnd) {
                    float wrapL = mBuffer[static_cast<size_t>(wrapPos) * 2];
                    float wrapR = mBuffer[static_cast<size_t>(wrapPos) * 2 + 1];
                    const float gOld = std::sqrt(fade);
                    const float gNew = std::sqrt(1.0f - fade);
                    sampleL = sampleL * gOld + wrapL * gNew;
                    sampleR = sampleR * gOld + wrapR * gNew;
                }
            }

            // Apply volume × mute × pan
            float gain = vol * muteGain;
            float outL = sampleL * gain * panL;
            float outR = sampleR * gain * panR;

            output[i * 2]     += outL;
            output[i * 2 + 1] += outR;

            // Track peak for metering
            float absL = std::abs(outL);
            float absR = std::abs(outR);
            if (absL > peakL) peakL = absL;
            if (absR > peakR) peakR = absR;
        }

        // Advance fractional playhead within loop region
        playHeadF = std::fmod(playHeadF + static_cast<float>(numFrames) * speed,
                              static_cast<float>(loopLen));
        if (playHeadF < 0.0f) playHeadF += static_cast<float>(loopLen);
        mPlayHeadF.store(playHeadF, std::memory_order_relaxed);
        mPlayHead.store(static_cast<int>(playHeadF), std::memory_order_relaxed);
        // Progress relative to loop region (0..1)
        mProgress.store(playHeadF / static_cast<float>(loopLen),
                        std::memory_order_relaxed);

        mVolumeSmoother.store(vol, std::memory_order_relaxed);
        mMuteGainSmoother.store(muteGain, std::memory_order_relaxed);
        mPanSmoother.store(panSmooth, std::memory_order_relaxed);

        // Update peak level (max of L/R, with decay)
        float currentPeak = mPeakLevel.load(std::memory_order_relaxed);
        float newPeak = std::max(peakL, peakR);
        // Exponential decay ~20dB/s
        currentPeak *= std::pow(0.9995f, static_cast<float>(numFrames));
        if (newPeak > currentPeak) currentPeak = newPeak;
        mPeakLevel.store(currentPeak, std::memory_order_relaxed);
    }

    // ========== Overdub (audio thread) ==========

    /**
     * @brief Overdub a stereo frame on top of existing content. RT-safe.
     *        Applies tanh soft-clip to prevent accumulation saturation.
     * @param pos Frame position in the buffer
     * @param left Left channel input
     * @param right Right channel input
     * @param gain Input gain (default 0.8)
     * @param decay Existing content decay (0 = keep all, 1 = replace all)
     */
    void overdubFrame(int pos, float left, float right, float gain, float decay) {
        if (pos < 0 || pos >= mLengthFrames.load(std::memory_order_relaxed)) return;

        size_t idx = static_cast<size_t>(pos) * 2;
        float existL = mBuffer[idx];
        float existR = mBuffer[idx + 1];

        float rawL = existL * (1.0f - decay) + left * gain;
        float rawR = existR * (1.0f - decay) + right * gain;

        // Soft-clip to prevent saturation from accumulative overdubs
        mBuffer[idx]     = tanhClip(rawL);
        mBuffer[idx + 1] = tanhClip(rawR);
    }

    // ========== Undo (UI thread) ==========

    /**
     * @brief Save current buffer state for undo. Call before overdub.
     * @return true if snapshot saved successfully
     */
    bool saveUndoSnapshot() {
        try {
            mUndoBuffer.resize(mBuffer.size());
            std::copy(mBuffer.begin(), mBuffer.end(), mUndoBuffer.begin());
            mHasUndo.store(true, std::memory_order_release);
            return true;
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief Restore buffer from undo snapshot.
     * @return true if restored successfully
     */
    bool restoreUndo() {
        if (!mHasUndo.load(std::memory_order_acquire)) return false;
        if (mUndoBuffer.size() != mBuffer.size()) return false;

        std::copy(mUndoBuffer.begin(), mUndoBuffer.end(), mBuffer.begin());
        mHasUndo.store(false, std::memory_order_release);
        return true;
    }

    // ========== Per-track playback control (lock-free) ==========

    void setPlaying(bool playing) { mPlaying.store(playing, std::memory_order_release); }
    bool isTrackPlaying() const { return mPlaying.load(std::memory_order_acquire); }
    float getProgress() const { return mProgress.load(std::memory_order_acquire); }
    int getPlayHead() const { return mPlayHead.load(std::memory_order_acquire); }
    /** Fractional playhead within the loop region. Used for cross-track phase-lock. */
    float getPlayHeadF() const { return mPlayHeadF.load(std::memory_order_acquire); }

    void resetPlayHead() {
        mPlayHead.store(0, std::memory_order_release);
        mPlayHeadF.store(0.0f, std::memory_order_release);
        mProgress.store(0.0f, std::memory_order_release);
    }

    /**
     * @brief Set the (fractional) playhead position. Used to phase-lock a freshly
     *        recorded overdub layer to the reference loop at finalize. mProgress
     *        self-corrects on the next mixInto() block (≤ one callback).
     */
    void setPlayHeadF(float pos) {
        if (pos < 0.0f) pos = 0.0f;
        mPlayHeadF.store(pos, std::memory_order_release);
        mPlayHead.store(static_cast<int>(pos), std::memory_order_release);
    }

    void setSpeed(float speed) { mSpeed.store(std::clamp(speed, 0.25f, 4.0f), std::memory_order_release); }
    float getSpeed() const { return mSpeed.load(std::memory_order_acquire); }

    /**
     * @brief Switch the loop seam profile for this track. RT-safe; takes effect on
     *        the next mixInto() block, so it can be toggled live on an already
     *        recorded track.
     *
     * Percussion: a near-instant equal-power crossfade (just enough to declick)
     * and NO tail-into-seam mixing — preserves the attack of a rhythmic transient
     * sitting near the loop point instead of smearing/flamming it.
     * Sustained: the long musical crossfade (default 50 ms) + tail mixing — masks
     * the seam for pads, delays and reverb tails.
     */
    void setPercussionMode(bool percussion) {
        if (percussion) {
            mSeamCrossfadeFrames.store(CROSSFADE_FRAMES, std::memory_order_release);  // ~2.7ms declick
            mTailEnabled.store(false, std::memory_order_release);
        } else {
            const int seamXf = static_cast<int>(DEFAULT_SEAM_CROSSFADE_MS * 0.001f
                                              * static_cast<float>(mSampleRate));
            mSeamCrossfadeFrames.store(std::max(CROSSFADE_FRAMES, seamXf),
                                       std::memory_order_release);
            mTailEnabled.store(true, std::memory_order_release);
        }
    }
    bool isPercussionMode() const { return !mTailEnabled.load(std::memory_order_acquire); }

    // ========== State queries (lock-free) ==========

    bool isActive() const { return mActive.load(std::memory_order_acquire); }
    bool isMuted() const { return mMuted.load(std::memory_order_acquire); }
    float getVolume() const { return mVolume.load(std::memory_order_acquire); }
    float getPeakLevel() const { return mPeakLevel.load(std::memory_order_acquire); }
    int getLengthFrames() const { return mLengthFrames.load(std::memory_order_acquire); }
    int getSampleRate() const { return mSampleRate; }
    bool hasUndo() const { return mHasUndo.load(std::memory_order_acquire); }
    int getCapacityFrames() const { return mCapacityFrames; }

    // ========== Parameter setters (lock-free, UI thread) ==========

    void setMuted(bool muted) { mMuted.store(muted, std::memory_order_release); }
    void setVolume(float vol) { mVolume.store(std::clamp(vol, 0.0f, 2.0f), std::memory_order_release); }
    void setPan(float pan) { mPan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_release); }
    float getPan() const { return mPan.load(std::memory_order_acquire); }

    // ========== Loop Region (lock-free) ==========

    void setLoopRegion(int start, int end) {
        int length = mLengthFrames.load(std::memory_order_acquire);
        if (length <= 0) return;
        int s = std::clamp(start, 0, length - 1);
        int e = (end <= 0) ? length : std::clamp(end, s + 1024, length);
        if (e - s < 1024) e = std::min(s + 1024, length);  // Enforce minimum
        mLoopStart.store(s, std::memory_order_release);
        mLoopEnd.store(e, std::memory_order_release);

        // Wrap playhead into new region if it exceeds the new loop length
        int newLen = e - s;
        float currentPos = mPlayHeadF.load(std::memory_order_relaxed);
        if (currentPos >= static_cast<float>(newLen)) {
            float wrapped = std::fmod(currentPos, static_cast<float>(newLen));
            mPlayHeadF.store(wrapped, std::memory_order_relaxed);
            mPlayHead.store(static_cast<int>(wrapped), std::memory_order_relaxed);
        }
    }

    void resetLoopRegion() {
        mLoopStart.store(0, std::memory_order_release);
        mLoopEnd.store(0, std::memory_order_release);
    }

    int getLoopStart() const { return mLoopStart.load(std::memory_order_acquire); }
    int getLoopEnd() const {
        int end = mLoopEnd.load(std::memory_order_acquire);
        return (end <= 0) ? mLengthFrames.load(std::memory_order_acquire) : end;
    }
    int getLoopLength() const { return getLoopEnd() - getLoopStart(); }

    /**
     * @brief Find the first and last frames of audible content (onset bounds),
     *        for trimming leading/trailing silence of a free take. UI/IO thread
     *        only — call after recording has stopped (no concurrent writes).
     *
     * The threshold is relative to the track's peak (so it adapts to recording
     * level), floored at a small absolute value to ignore noise. If the track is
     * silent, returns false and the bounds span the whole buffer.
     *
     * @param thresholdRatio Fraction of peak amplitude that counts as content
     *                       (e.g. 0.03 = 3% of peak ≈ -30 dB below peak).
     * @param outFirst       First content frame (inclusive).
     * @param outLast        One past the last content frame (exclusive).
     * @return true if content was found.
     */
    bool findContentBounds(float thresholdRatio, int& outFirst, int& outLast) const {
        const int len = mLengthFrames.load(std::memory_order_acquire);
        outFirst = 0;
        outLast = len;
        if (len <= 0 || mBuffer.size() < static_cast<size_t>(len) * 2) return false;

        float peak = 0.0f;
        for (int i = 0; i < len; ++i) {
            const float m = std::max(std::abs(mBuffer[static_cast<size_t>(i) * 2]),
                                     std::abs(mBuffer[static_cast<size_t>(i) * 2 + 1]));
            if (m > peak) peak = m;
        }
        if (peak <= 0.0f) return false;

        const float threshold = std::max(peak * thresholdRatio, 1.0e-4f);

        int first = 0;
        while (first < len) {
            const float m = std::max(std::abs(mBuffer[static_cast<size_t>(first) * 2]),
                                     std::abs(mBuffer[static_cast<size_t>(first) * 2 + 1]));
            if (m > threshold) break;
            ++first;
        }
        int last = len - 1;
        while (last > first) {
            const float m = std::max(std::abs(mBuffer[static_cast<size_t>(last) * 2]),
                                     std::abs(mBuffer[static_cast<size_t>(last) * 2 + 1]));
            if (m > threshold) break;
            --last;
        }
        outFirst = first;
        outLast = std::min(last + 1, len);  // exclusive end
        return outLast > outFirst;
    }

    /**
     * @brief Detect note onsets (transients) via energy flux, for deriving a
     *        free take's tempo from its RHYTHM (inter-onset intervals) rather
     *        than its total length. UI/IO thread only — call after recording has
     *        stopped (no concurrent writes).
     *
     * Algorithm (lightweight; RT not required here):
     *   1. Short-time energy per window (sum of L²+R² over `hopFrames`).
     *   2. Positive log-energy flux (rectified first difference) — log domain
     *      keeps soft and loud hits comparable.
     *   3. Adaptive peak-pick: a window is an onset if its flux exceeds a local
     *      moving-average threshold, is a local maximum, and is ≥ ~50ms after the
     *      previous onset (rejects flams / double triggers).
     *
     * @param outOnsets   Caller buffer for onset frame positions (ascending).
     * @param maxOnsets   Capacity of outOnsets.
     * @param hopFrames   Analysis window size in frames (e.g. 256 ≈ 5.3ms@48k).
     * @param sensitivity >1 = more onsets (lower threshold), <1 = fewer (~1.0 default).
     * @return number of onsets written (0 if silent / too short / invalid).
     */
    int detectOnsets(int* outOnsets, int maxOnsets,
                     int hopFrames, float sensitivity) const {
        if (!outOnsets || maxOnsets <= 0) return 0;
        if (hopFrames < 32) hopFrames = 32;
        const int len = mLengthFrames.load(std::memory_order_acquire);
        if (len <= hopFrames * 4) return 0;
        if (mBuffer.size() < static_cast<size_t>(len) * 2) return 0;

        const int numWin = len / hopFrames;
        if (numWin < 4) return 0;

        std::vector<float> energy, smoothed, flux;
        try {
            energy.assign(static_cast<size_t>(numWin), 0.0f);
            smoothed.assign(static_cast<size_t>(numWin), 0.0f);
            flux.assign(static_cast<size_t>(numWin), 0.0f);
        } catch (...) {
            return 0;
        }

        // 1) Short-time energy per window.
        for (int w = 0; w < numWin; ++w) {
            const int base = w * hopFrames;
            float e = 0.0f;
            for (int i = 0; i < hopFrames; ++i) {
                const float l = mBuffer[static_cast<size_t>(base + i) * 2];
                const float r = mBuffer[static_cast<size_t>(base + i) * 2 + 1];
                e += l * l + r * r;
            }
            energy[w] = e;
        }

        // 1b) Smooth the energy (±2-window moving average) BEFORE the flux. A
        // sustained/tonal note whose period exceeds the hop makes the raw windowed
        // energy oscillate every block, which the flux reads as a stream of false
        // onsets (a held synth pad reported >100 onsets / 6 s). Averaging over a few
        // windows flattens that wobble while a real transient still steps the
        // smoothed energy up sharply.
        const int sm = 2;
        for (int w = 0; w < numWin; ++w) {
            const int a = std::max(0, w - sm);
            const int b = std::min(numWin - 1, w + sm);
            float sum = 0.0f;
            for (int k = a; k <= b; ++k) sum += energy[k];
            smoothed[w] = sum / static_cast<float>(b - a + 1);
        }

        // 2) Positive log-energy flux on the smoothed envelope.
        for (int w = 1; w < numWin; ++w) {
            const float d = std::log(smoothed[w] + 1e-9f) - std::log(smoothed[w - 1] + 1e-9f);
            flux[w] = (d > 0.0f) ? d : 0.0f;
        }

        // 3) Adaptive peak-pick.
        const float mult = (sensitivity > 0.01f) ? (1.0f / sensitivity) : 1.0f;
        // Absolute log-energy flux floor (≈ +3 dB jump). The adaptive threshold
        // alone adapts UP with the local flux, so sustained/tonal material (whose
        // windowed energy wobbles every block) trips it constantly — a take of a
        // held synth pad reported ~88 onsets / 5 s. A real transient jumps energy
        // by several dB (flux ≫ this); steady-tone wobble does not. Scales with the
        // sensitivity knob so the param still works.
        const float minFlux = 0.5f * mult;
        const int half = 8;  // local-mean window (± windows)
        const int minGapWin =
            std::max(1, static_cast<int>(0.05f * static_cast<float>(mSampleRate)
                                         / static_cast<float>(hopFrames)));
        int count = 0;
        int lastOnsetWin = -minGapWin - 1;
        for (int w = 1; w < numWin - 1 && count < maxOnsets; ++w) {
            const float f = flux[w];
            if (f <= minFlux) continue;
            const int a = std::max(1, w - half);
            const int b = std::min(numWin - 1, w + half);
            float sum = 0.0f;
            int n = 0;
            for (int k = a; k < b; ++k) { sum += flux[k]; ++n; }
            const float localMean = (n > 0) ? sum / static_cast<float>(n) : 0.0f;
            const float threshold = localMean * 1.5f * mult + 1.0e-4f;
            if (f > threshold && f >= flux[w - 1] && f > flux[w + 1]
                && (w - lastOnsetWin) >= minGapWin) {
                outOnsets[count++] = w * hopFrames;
                lastOnsetWin = w;
            }
        }
        return count;
    }

    /**
     * @brief Bar-snap + seam-bake a free take's loop in one RT-safe pass.
     *        UI/IO thread only (mirrors trimToLength()'s pause/fence/realloc
     *        pattern). No-op semantics handled by the AudioLooper guard.
     *
     *   1. SEAM BAKE (when `tailFrames` > 0): overdub the natural continuation
     *      past `loopEnd` into the loop START with a quasi-exponential decay, so
     *      a sound still ringing across the seam bleeds in instead of being cut.
     *      Uses the ORIGINAL recorded content beyond loopEnd (before padding).
     *   2. PAD: if `loopEnd` extends past the recording (user finished a hair
     *      early → bar rounded up), grow the buffer with trailing silence so the
     *      loop closes exactly on the grid.
     *   3. REGION: set the loop region to [loopStart, loopEnd) and restart the
     *      playhead at the region start.
     *
     * @return true on success; false on degenerate args / alloc failure.
     */
    bool finalizeFreeLoop(int loopStart, int loopEnd, int tailFrames) {
        const int origLen = mLengthFrames.load(std::memory_order_acquire);
        if (origLen <= 0) return false;
        if (loopStart < 0) loopStart = 0;
        if (loopEnd <= loopStart) return false;
        if (mBuffer.size() < static_cast<size_t>(origLen) * 2) return false;

        const bool wasPlaying = mPlaying.load(std::memory_order_acquire);
        mPlaying.store(false, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        waitForRenderIdle();  // no mixInto may read mBuffer while we bake/realloc it

        // 1) Seam wrap-mix from the continuation past loopEnd into the loop start.
        if (tailFrames > 0) {
            const int loopLen = loopEnd - loopStart;
            int tail = std::min(tailFrames, loopLen / 2);
            tail = std::min(tail, origLen - loopEnd);  // only what we actually recorded
            const float invTail = (tail > 0) ? 1.0f / static_cast<float>(tail) : 0.0f;
            for (int i = 0; i < tail; ++i) {
                const int src = loopEnd + i;
                const int dst = loopStart + i;
                if (src >= origLen) break;
                float fade = 1.0f - static_cast<float>(i) * invTail;
                fade = fade * fade;  // (1-t)^2 decay, matches record-time wrap-mix
                const size_t di = static_cast<size_t>(dst) * 2;
                const size_t si = static_cast<size_t>(src) * 2;
                mBuffer[di]     = tanhClip(mBuffer[di]     + mBuffer[si]     * fade);
                mBuffer[di + 1] = tanhClip(mBuffer[di + 1] + mBuffer[si + 1] * fade);
            }
        }

        // 2) Pad with silence if the snapped loop end runs past the recording.
        if (loopEnd > origLen) {
            try {
                std::vector<float> grown(static_cast<size_t>(loopEnd) * 2, 0.0f);
                std::copy(mBuffer.begin(),
                          mBuffer.begin() + static_cast<size_t>(origLen) * 2,
                          grown.begin());
                mBuffer.swap(grown);
            } catch (...) {
                if (wasPlaying) mPlaying.store(true, std::memory_order_release);
                return false;
            }
            mCapacityFrames = loopEnd;
            mLoopCapacityFrames = loopEnd;
            mLengthFrames.store(loopEnd, std::memory_order_release);
            mWriteHead.store(loopEnd, std::memory_order_release);
            mTailFrames.store(0, std::memory_order_release);  // no dedicated tail region
        }

        // 3) Set the bar-snapped loop region and restart from the top.
        const int len = mLengthFrames.load(std::memory_order_acquire);
        const int s = std::clamp(loopStart, 0, len - 1);
        const int e = std::clamp(loopEnd, s + 1, len);
        mLoopStart.store(s, std::memory_order_release);
        mLoopEnd.store(e, std::memory_order_release);
        mPlayHead.store(0, std::memory_order_relaxed);
        mPlayHeadF.store(0.0f, std::memory_order_relaxed);
        mProgress.store(0.0f, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (wasPlaying) mPlaying.store(true, std::memory_order_release);
        return true;
    }

    // ========== Buffer access (for export — snapshot copy first!) ==========

    const float* data() const { return mBuffer.data(); }

private:
    static inline float tanhClip(float x) {
        return std::tanh(x * 0.666f) * 1.5f;
    }

    // RT-safety: clear()/trimToLength()/finalizeFreeLoop() free or realloc mBuffer
    // from the UI/IO thread, while mixInto() (audio thread) reads mBuffer across a
    // whole block. mRendering marks "audio thread is inside mixInto for this
    // track"; the freeing paths set mPlaying=false then spin until it clears, so
    // the buffer is never deallocated mid-render (fixes a use-after-free crash on
    // clearAll() while a track is playing).
    std::atomic<bool> mRendering{false};

    struct RenderScope {
        std::atomic<bool>& flag;
        explicit RenderScope(std::atomic<bool>& f) : flag(f) {
            flag.store(true, std::memory_order_seq_cst);
        }
        ~RenderScope() { flag.store(false, std::memory_order_release); }
    };

    /**
     * @brief UI/IO thread: block until the audio thread is not mid-mixInto() for
     *        this track. Call AFTER setting mPlaying=false (so mixInto bails on the
     *        next block) and BEFORE freeing/reallocating mBuffer. Bounded by one
     *        audio callback (~a few ms) — not RT-safe, never call from audio thread.
     */
    void waitForRenderIdle() const {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (mRendering.load(std::memory_order_acquire)) {
            // brief spin; the audio thread clears the flag at the end of mixInto()
        }
    }

    // Audio data
    std::vector<float> mBuffer;        // Stereo interleaved, heap (loop + tail)
    std::vector<float> mUndoBuffer;    // Lazy undo snapshot
    int mCapacityFrames{0};            // Buffer length in frames (= loop body; no tail region)
    int mLoopCapacityFrames{0};        // Musical loop capacity (excludes tail)
    int mSampleRate{48000};
    std::atomic<int> mTailFrames{0};           // Captured tail length (decay region)
    std::atomic<int> mSeamCrossfadeFrames{128}; // Equal-power crossfade window @ loop seam
    std::atomic<bool> mTailEnabled{true};      // Tail-into-seam mixing (off = percussion)

    // State (atomic for cross-thread access)
    std::atomic<int> mLengthFrames{0};
    std::atomic<int> mWriteHead{0};
    std::atomic<bool> mActive{false};
    std::atomic<bool> mMuted{false};
    std::atomic<float> mVolume{1.0f};
    std::atomic<float> mPan{0.0f};
    std::atomic<float> mPeakLevel{0.0f};
    std::atomic<float> mVolumeSmoother{1.0f};
    std::atomic<float> mMuteGainSmoother{1.0f};  // 1.0=unmuted, smooths to 0.0 on mute
    std::atomic<float> mPanSmoother{0.0f};       // Smoothed pan value
    std::atomic<bool> mHasUndo{false};

    // Per-track playback (independent playhead)
    std::atomic<int> mPlayHead{0};
    std::atomic<float> mPlayHeadF{0.0f};  // Fractional playhead for speed != 1.0
    std::atomic<bool> mPlaying{false};
    std::atomic<float> mProgress{0.0f};
    std::atomic<float> mSpeed{1.0f};      // Playback speed: 0.25..4.0

    // Loop region (defaults to full buffer)
    std::atomic<int> mLoopStart{0};       // First frame of loop region
    std::atomic<int> mLoopEnd{0};         // Last frame (0 = use mLengthFrames)
};
