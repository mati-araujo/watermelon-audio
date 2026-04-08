#pragma once

#include "TrackBuffer.h"
#include "WavFile.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include "../platform/Logger.h"

#define P12_LOG_TAG "P12.Looper"
#define P12_LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, P12_LOG_TAG, __VA_ARGS__)
#define P12_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, P12_LOG_TAG, __VA_ARGS__)

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
    static constexpr int CLICK_DURATION_FRAMES = 480;   // ~10ms @ 48kHz
    static constexpr int CLICK_FADE_FRAMES = 120;       // fade-out last 25%
    static constexpr int MAX_BUFFER_FRAMES = 1024;      // Max Oboe buffer size

    AudioLooper() = default;
    ~AudioLooper() = default;

    // ========== Audio thread (RT-safe) ==========

    void process(float* audioData, int numFrames) {
        if (!mEnabled.load(std::memory_order_acquire)) return;

        int recTrack = mRecordingTrack.load(std::memory_order_acquire);
        bool overdubbing = mOverdubbing.load(std::memory_order_acquire);

        // ---- CAPTURE (recording or overdub) ----
        if (recTrack >= 0 && recTrack < MAX_TRACKS) {
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
                // Normal recording
                int capacity = mRecordCapacityFrames.load(std::memory_order_relaxed);
                int remaining = mRecordFramesRemaining.load(std::memory_order_relaxed);
                for (int i = 0; i < numFrames && remaining > 0; ++i) {
                    mTracks[recTrack].writeFrame(audioData[i * 2], audioData[i * 2 + 1]);
                    remaining--;
                }
                mRecordFramesRemaining.store(remaining, std::memory_order_relaxed);

                // Update recording progress (0.0 → 1.0)
                if (capacity > 0) {
                    float recProg = 1.0f - (static_cast<float>(remaining) / static_cast<float>(capacity));
                    mRecordProgress.store(recProg, std::memory_order_relaxed);
                }

                // Auto-stop when recording complete (only in fixed-length mode)
                if (remaining <= 0 && !mFreeLength.load(std::memory_order_relaxed)) {
                    finalizeCurrentRecording();
                }
            }
        }

        // ---- PLAYBACK (mix all tracks into temp buffer, apply master volume) ----
        int framesToProcess = std::min(numFrames, MAX_BUFFER_FRAMES);
        std::memset(mLooperMixBuf, 0, sizeof(float) * framesToProcess * 2);

        for (int t = 0; t < MAX_TRACKS; ++t) {
            mTracks[t].mixInto(mLooperMixBuf, framesToProcess);
        }

        // Apply master volume with smoothing, then add to main output
        float masterVol = mMasterVolSmoother.load(std::memory_order_relaxed);
        float targetMaster = mMasterVolume.load(std::memory_order_acquire);
        constexpr float kSmooth = 0.995f;

        for (int i = 0; i < framesToProcess; ++i) {
            masterVol = kSmooth * masterVol + (1.0f - kSmooth) * targetMaster;
            audioData[i * 2]     += mLooperMixBuf[i * 2]     * masterVol;
            audioData[i * 2 + 1] += mLooperMixBuf[i * 2 + 1] * masterVol;
        }
        mMasterVolSmoother.store(masterVol, std::memory_order_relaxed);

        // ---- METRONOME CLICK (pre-count beat indicator — NOT affected by master volume) ----
        if (mClickRemaining.load(std::memory_order_relaxed) > 0) {
            int remaining = mClickRemaining.load(std::memory_order_relaxed);
            int phase = mClickPhase.load(std::memory_order_relaxed);
            float freq = mClickFreq.load(std::memory_order_relaxed);
            float gain = mClickGain.load(std::memory_order_relaxed);
            for (int i = 0; i < numFrames && remaining > 0; ++i) {
                // Sine burst with envelope (fade out last 25%)
                float env = (remaining < CLICK_FADE_FRAMES)
                    ? static_cast<float>(remaining) / static_cast<float>(CLICK_FADE_FRAMES)
                    : 1.0f;
                float sample = std::sin(2.0f * static_cast<float>(M_PI) * freq
                    * static_cast<float>(phase) / 48000.0f) * gain * env;
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

    bool prepareTrack(int trackIndex, int lengthFrames, int sampleRate) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return false;
        if (lengthFrames <= 0) return false;

        // Each track can have its own length — no master loop enforcement
        size_t needed = static_cast<size_t>(lengthFrames) * 2 * sizeof(float);
        size_t currentUsage = getTotalAllocatedBytes();
        size_t trackCurrent = mTracks[trackIndex].allocatedBytes();
        if (currentUsage - trackCurrent + needed > MEMORY_BUDGET_BYTES) {
            return false;
        }

        size_t allocated = mTracks[trackIndex].allocate(lengthFrames, sampleRate);
        return allocated > 0;
    }

    void startRecording(int trackIndex) {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        int capacity = mTracks[trackIndex].getCapacityFrames();
        if (capacity <= 0) return;
        mRecordCapacityFrames.store(capacity, std::memory_order_release);
        mRecordFramesRemaining.store(capacity, std::memory_order_release);
        mRecordProgress.store(0.0f, std::memory_order_release);
        mOverdubbing.store(false, std::memory_order_release);
        mRecordingTrack.store(trackIndex, std::memory_order_release);
        mEnabled.store(true, std::memory_order_release);
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
     * @brief Export mix of all active tracks to a WAV file.
     *        Snapshot-copies buffers to avoid data race with audio thread.
     * @param filePath Output file path
     * @return true if successful
     */
    bool exportMix(const char* filePath) const {
        // Find longest active track
        int maxLen = 0;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isActive()) {
                int len = mTracks[i].getLengthFrames();
                if (len > maxLen) maxLen = len;
            }
        }
        if (maxLen == 0) return false;

        // Snapshot-copy: mix all tracks into a temporary buffer
        std::vector<float> mixBuffer(static_cast<size_t>(maxLen) * 2, 0.0f);

        for (int t = 0; t < MAX_TRACKS; ++t) {
            if (!mTracks[t].isActive()) continue;
            const float* trackData = mTracks[t].data();
            int trackLen = mTracks[t].getLengthFrames();
            float vol = mTracks[t].getVolume();
            bool muted = mTracks[t].isMuted();
            if (muted) continue;

            // Equal-power pan
            float pan = mTracks[t].getPan();
            float angle = (pan + 1.0f) * 0.25f * static_cast<float>(M_PI);
            float panL = std::cos(angle);
            float panR = std::sin(angle);

            for (int i = 0; i < maxLen; ++i) {
                int pos = i % trackLen;  // Loop shorter tracks
                float sampleL = trackData[pos * 2] * vol * panL;
                float sampleR = trackData[pos * 2 + 1] * vol * panR;
                mixBuffer[i * 2] += sampleL;
                mixBuffer[i * 2 + 1] += sampleR;
            }
        }

        // Soft-clip the final mix
        for (size_t i = 0; i < mixBuffer.size(); ++i) {
            mixBuffer[i] = std::tanh(mixBuffer[i] * 0.666f) * 1.5f;
        }

        int sr = mTracks[0].getSampleRate();
        return wav::writeWav(filePath, mixBuffer.data(), maxLen, sr);
    }

    /**
     * @brief Export a single track to a WAV file.
     * @param trackIndex Track to export (0-7)
     * @param filePath Output file path
     * @return true if successful
     */
    bool exportTrack(int trackIndex, const char* filePath) const {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return false;
        if (!mTracks[trackIndex].isActive()) return false;

        const float* data = mTracks[trackIndex].data();
        int len = mTracks[trackIndex].getLengthFrames();
        int sr = mTracks[trackIndex].getSampleRate();

        // If a custom loop region is defined, export only the region
        int regionStart = mTracks[trackIndex].getLoopStart();
        int regionEnd = mTracks[trackIndex].getLoopEnd();
        if (regionStart > 0 || regionEnd < len) {
            int regionLen = regionEnd - regionStart;
            if (regionLen > 0) {
                const float* regionData = data + (static_cast<size_t>(regionStart) * 2);
                return wav::writeWav(filePath, regionData, regionLen, sr);
            }
        }
        return wav::writeWav(filePath, data, len, sr);
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

        P12_LOGD("[P12] importTrack: reading %s", filePath);
        wav::WavData wavData = wav::readWav(filePath);
        if (wavData.numFrames <= 0) {
            P12_LOGE("[P12] importTrack FAILED: readWav returned 0 frames (unsupported format or corrupt file)");
            return false;
        }
        P12_LOGD("[P12] importTrack: %d frames, %dHz, %d ch", wavData.numFrames, wavData.sampleRate, wavData.numChannels);

        // Resample if source sample rate differs from target (e.g., 44100 → 48000)
        bool needsResample = (wavData.sampleRate > 0 && wavData.sampleRate != sampleRate);
        int outputFrames = wavData.numFrames;
        std::vector<float> resampledBuffer;

        if (needsResample) {
            double ratio = static_cast<double>(sampleRate) / static_cast<double>(wavData.sampleRate);
            outputFrames = static_cast<int>(std::ceil(wavData.numFrames * ratio));
            P12_LOGD("[P12] Resampling %dHz → %dHz (ratio=%.4f, %d → %d frames)",
                     wavData.sampleRate, sampleRate, ratio, wavData.numFrames, outputFrames);

            resampledBuffer.resize(static_cast<size_t>(outputFrames) * 2);
            for (int i = 0; i < outputFrames; ++i) {
                // Source position (fractional)
                double srcPos = i / ratio;
                int srcIdx0 = static_cast<int>(srcPos);
                int srcIdx1 = std::min(srcIdx0 + 1, wavData.numFrames - 1);
                float frac = static_cast<float>(srcPos - srcIdx0);

                // Linear interpolation
                resampledBuffer[i * 2]     = wavData.buffer[srcIdx0 * 2]     * (1.0f - frac)
                                           + wavData.buffer[srcIdx1 * 2]     * frac;
                resampledBuffer[i * 2 + 1] = wavData.buffer[srcIdx0 * 2 + 1] * (1.0f - frac)
                                           + wavData.buffer[srcIdx1 * 2 + 1] * frac;
            }
        }

        const float* srcBuffer = needsResample ? resampledBuffer.data() : wavData.buffer.data();

        // Check memory budget
        size_t needed = static_cast<size_t>(outputFrames) * 2 * sizeof(float);
        size_t currentUsage = getTotalAllocatedBytes();
        size_t trackCurrent = mTracks[trackIndex].allocatedBytes();
        if (currentUsage - trackCurrent + needed > MEMORY_BUDGET_BYTES) {
            P12_LOGE("[P12] importTrack FAILED: memory budget exceeded (need %zu, budget %zu, used %zu)",
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
     * @param index Track index
     * @param outBins Output array (caller-allocated)
     * @param numBins Number of bins to generate
     * @return Number of bins written
     */
    int getTrackWaveform(int index, float* outBins, int numBins) const {
        if (index < 0 || index >= MAX_TRACKS || !outBins || numBins <= 0) return 0;
        if (!mTracks[index].isActive()) return 0;
        const float* data = mTracks[index].data();
        int length = mTracks[index].getLengthFrames();
        if (length <= 0) return 0;

        int framesPerBin = length / numBins;
        if (framesPerBin <= 0) framesPerBin = 1;

        for (int bin = 0; bin < numBins; ++bin) {
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
        }
        return numBins;
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

    /** Trigger a metronome click. Call from UI thread during pre-count. */
    void triggerClick(bool isDownbeat) {
        mClickFreq.store(isDownbeat ? 1200.0f : 900.0f, std::memory_order_relaxed);
        mClickGain.store(isDownbeat ? 0.35f : 0.25f, std::memory_order_relaxed);
        mClickPhase.store(0, std::memory_order_relaxed);
        mClickRemaining.store(CLICK_DURATION_FRAMES, std::memory_order_release);
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
    /** Finalize a recording in progress — set master loop, auto-start playback. */
    void finalizeCurrentRecording() {
        int recTrack = mRecordingTrack.load(std::memory_order_acquire);
        if (recTrack < 0 || recTrack >= MAX_TRACKS) return;

        mTracks[recTrack].finalizeRecording();
        mTracks[recTrack].resetPlayHead();
        mTracks[recTrack].setPlaying(true);
        mRecordingTrack.store(-1, std::memory_order_release);

        // Also start any other active tracks that should be playing
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isActive() && !mTracks[i].isTrackPlaying()) {
                mTracks[i].setPlaying(true);
            }
        }
    }

    bool hasAnyActiveTracks() const {
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (mTracks[i].isActive()) return true;
        }
        return false;
    }

    size_t getTotalAllocatedBytes() const {
        size_t total = 0;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            total += mTracks[i].allocatedBytes();
        }
        return total;
    }

    TrackBuffer mTracks[MAX_TRACKS];

    std::atomic<int> mRecordingTrack{-1};
    std::atomic<bool> mOverdubbing{false};
    std::atomic<int> mRecordFramesRemaining{0};
    std::atomic<int> mRecordCapacityFrames{0};
    std::atomic<bool> mEnabled{false};
    std::atomic<bool> mFreeLength{false};
    std::atomic<float> mRecordProgress{0.0f};
    std::atomic<float> mOverdubGain{0.8f};
    std::atomic<float> mOverdubDecay{0.0f};

    // Master volume (applied to combined looper output)
    std::atomic<float> mMasterVolume{1.0f};
    std::atomic<float> mMasterVolSmoother{1.0f};
    float mLooperMixBuf[MAX_BUFFER_FRAMES * 2] = {};  // Pre-allocated temp buffer for mixing

    // Metronome click state (lock-free)
    std::atomic<int> mClickRemaining{0};
    std::atomic<int> mClickPhase{0};
    std::atomic<float> mClickFreq{1000.0f};
    std::atomic<float> mClickGain{0.3f};
};
