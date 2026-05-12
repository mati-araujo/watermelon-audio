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
        const int totalFrames = loopFrames + tailFrames;
        try {
            mBuffer.resize(static_cast<size_t>(totalFrames) * 2, 0.0f);
        } catch (...) {
            return 0;
        }
        mCapacityFrames = totalFrames;
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

        // Step 3: Now safe to deallocate — audio thread won't access buffer
        // because mixInto() returns early when !mPlaying || !mActive || length<=0.
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
        // Retain heap capacity to avoid fragmentation across many record/clear cycles.
        // size() drops to 0; capacity() stays. Re-recording skips the allocation cost as
        // long as the new track length fits in the existing capacity. allocate() will
        // resize() up if needed; the only way to truly free is destruction of TrackBuffer.
        mBuffer.clear();
        mUndoBuffer.clear();
        // Note: mCapacityFrames is intentionally NOT reset — it tracks the LOGICAL track
        // capacity (set in allocate()), which becomes meaningless once length=0. Reset
        // here prevents writeFrame() from accepting frames before next allocate().
        mCapacityFrames = 0;
        mLoopCapacityFrames = 0;
        mTailFrames.store(0, std::memory_order_relaxed);
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

    // ========== Playback (audio thread) ==========

    /**
     * @brief Mix this track into an output buffer using the track's own playhead.
     *        Applies crossfade at loop wrap-around point. RT-safe.
     *        Only produces output if both mActive and mPlaying are true.
     * @param output Stereo interleaved output buffer to ADD into (not overwrite)
     * @param numFrames Number of frames to process
     */
    void mixInto(float* output, int numFrames) {
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

        // Tail mixing: if a tail region was captured, mix it into the start of
        // each loop iteration with a linear fade-out. This preserves the natural
        // decay of sustained notes at the loop seam (delays, reverbs, pads).
        //
        // Tail source = the frames recorded immediately AFTER the user's current
        // loopEnd. For full-buffer playback (loopEnd == loopCap) this is the
        // dedicated tail region [loopCap, loopCap + tailFrames). For custom loop
        // regions, it's the "what came next in the original take" [loopEnd, ...) —
        // which is musically the right continuation at that seam.
        const int tailFrames    = mTailFrames.load(std::memory_order_acquire);
        const int tailSrcStart  = loopEnd;
        const int tailSrcAvail  = std::max(0, mCapacityFrames - tailSrcStart);
        const int effectiveTail = std::min(tailFrames, tailSrcAvail);
        const bool tailActive   = (effectiveTail > 0);
        const float invTail = tailActive ? 1.0f / static_cast<float>(effectiveTail) : 0.0f;

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

            // Tail mixing: in the first `effectiveTail` frames of each iteration,
            // sum samples from the captured tail region with a fade-out that
            // approximates natural exponential decay. Linear fade would drop the
            // sustain too aggressively at the start (where ears are most sensitive);
            // (1-t)^2 stays close to 1.0 for the first ~30% then accelerates.
            // For full-buffer playback the tail source is [loopCap, loopCap + tail);
            // for a custom loop region it's the "post-region" frames in the original
            // take. Either way this makes the seam perceptually continuous.
            if (tailActive) {
                int tailIdx = static_cast<int>(regionPos);
                if (tailIdx < effectiveTail) {
                    int tailBufIdx = tailSrcStart + tailIdx;
                    const float lin = 1.0f - (static_cast<float>(tailIdx) * invTail);
                    const float tailFade = lin * lin;  // quasi-exponential decay shape
                    sampleL += mBuffer[static_cast<size_t>(tailBufIdx) * 2]     * tailFade;
                    sampleR += mBuffer[static_cast<size_t>(tailBufIdx) * 2 + 1] * tailFade;
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

    void resetPlayHead() {
        mPlayHead.store(0, std::memory_order_release);
        mPlayHeadF.store(0.0f, std::memory_order_release);
        mProgress.store(0.0f, std::memory_order_release);
    }

    void setSpeed(float speed) { mSpeed.store(std::clamp(speed, 0.25f, 4.0f), std::memory_order_release); }
    float getSpeed() const { return mSpeed.load(std::memory_order_acquire); }

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

    // ========== Buffer access (for export — snapshot copy first!) ==========

    const float* data() const { return mBuffer.data(); }

private:
    static inline float tanhClip(float x) {
        return std::tanh(x * 0.666f) * 1.5f;
    }

    // Audio data
    std::vector<float> mBuffer;        // Stereo interleaved, heap (loop + tail)
    std::vector<float> mUndoBuffer;    // Lazy undo snapshot
    int mCapacityFrames{0};            // Total frames including tail (= loopCap + tail)
    int mLoopCapacityFrames{0};        // Musical loop capacity (excludes tail)
    int mSampleRate{48000};
    std::atomic<int> mTailFrames{0};           // Captured tail length (decay region)
    std::atomic<int> mSeamCrossfadeFrames{128}; // Equal-power crossfade window @ loop seam

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
