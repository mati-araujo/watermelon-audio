#pragma once

// ============================================================================
// TrackStorage — the audio-sample storage backing a single looper track.
//
// Two interchangeable implementations selected at compile time (plan §3.1, F2.2):
//   - default:                    dense std::vector<float> (legacy behaviour).
//   - -D WM_LOOPER_CHUNKED_BUFFER: paged ChunkedAudioBuffer (silence = no memory,
//                                  O(pages) trim/pad, copy-on-write undo).
//
// TrackBuffer talks only to this uniform API, so the RT/analysis code has no
// #ifdefs. During the transition dense stays the default; the chunked path is
// exercised by a parallel test target built with the flag. The final F2 step
// flips the default (and renames the fallback flag to WM_LOOPER_DENSE_BUFFER).
// ============================================================================

#include <algorithm>
#include <cstddef>
#include <vector>

#ifdef WM_LOOPER_CHUNKED_BUFFER
#include "ChunkedAudioBuffer.h"
#include "ChunkPool.h"
#endif

namespace wm {

class TrackStorage {
public:
    // ---- lifecycle (UI/IO thread) ----

    /** (Re)initialise to `frames` of silence. Returns logical bytes (frames×2×4). */
    size_t allocate(int frames) {
        if (frames < 0) frames = 0;
        mCapacity = frames;
#ifdef WM_LOOPER_CHUNKED_BUFFER
        // Prefill the pool to cover the whole capacity so the audio thread never
        // has to allocate while recording within [0, frames). (Working-set sizing
        // + IO refill to realise the live-recording RAM win is a follow-up.)
        mPool.prefill(static_cast<size_t>(pageCountFor(frames)));
        mChunked.setPool(&mPool);
        mChunked.reset(frames);
#else
        mBuffer.assign(static_cast<size_t>(frames) * 2, 0.0f);
#endif
        return static_cast<size_t>(frames) * 2 * sizeof(float);
    }

    /** Release all memory / return chunks to the pool; go empty. */
    void release() {
        mCapacity = 0;
#ifdef WM_LOOPER_CHUNKED_BUFFER
        mChunked.clear();
#else
        std::vector<float>().swap(mBuffer);
        std::vector<float>().swap(mUndoBuffer);
        mHasUndo = false;
#endif
    }

    /** Truncate to `frames`. O(pages) with chunks; realloc+copy when dense. */
    void trim(int frames) {
        if (frames < 0) frames = 0;
        mCapacity = frames;
#ifdef WM_LOOPER_CHUNKED_BUFFER
        mChunked.trim(frames);
#else
        std::vector<float> trimmed(mBuffer.begin(),
                                   mBuffer.begin() + std::min(mBuffer.size(),
                                       static_cast<size_t>(frames) * 2));
        mBuffer.swap(trimmed);
#endif
    }

    /** Grow to `frames`, the new region silent. Preserves existing content. */
    void padWithSilence(int frames) {
        if (frames <= mCapacity) return;
        mCapacity = frames;
#ifdef WM_LOOPER_CHUNKED_BUFFER
        mChunked.pad(frames);
#else
        mBuffer.resize(static_cast<size_t>(frames) * 2, 0.0f);
#endif
    }

    // ---- access ----

    int capacityFrames() const { return mCapacity; }

    size_t allocatedBytes() const {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        return mChunked.allocatedBytes();
#else
        return mBuffer.capacity() * sizeof(float) + mUndoBuffer.capacity() * sizeof(float);
#endif
    }

    /** Write one frame (materialising the page with chunks). false if OOB / pool empty. */
    bool put(int pos, float l, float r) {
        if (pos < 0 || pos >= mCapacity) return false;
#ifdef WM_LOOPER_CHUNKED_BUFFER
        return mChunked.writeFrame(pos, l, r);
#else
        mBuffer[static_cast<size_t>(pos) * 2]     = l;
        mBuffer[static_cast<size_t>(pos) * 2 + 1] = r;
        return true;
#endif
    }

    /** Read one frame; zeros for silent pages / out of range. */
    void get(int pos, float& l, float& r) const {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        mChunked.readFrame(pos, l, r);
#else
        if (pos < 0 || pos >= mCapacity) { l = 0.0f; r = 0.0f; return; }
        l = mBuffer[static_cast<size_t>(pos) * 2];
        r = mBuffer[static_cast<size_t>(pos) * 2 + 1];
#endif
    }

    /** Single-channel sample (for interpolation). */
    float sample(int pos, int channel) const {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        return mChunked.sampleAt(pos, channel);
#else
        if (pos < 0 || pos >= mCapacity) return 0.0f;
        return mBuffer[static_cast<size_t>(pos) * 2 + channel];
#endif
    }

    /**
     * @brief Contiguous run starting at `pos`: pointer + frame count. nullptr =
     *        silent run (emit `runFrames` zeros). Dense returns the whole tail.
     */
    const float* run(int pos, int& runFrames) const {
        if (pos < 0 || pos >= mCapacity) { runFrames = 0; return nullptr; }
#ifdef WM_LOOPER_CHUNKED_BUFFER
        return mChunked.contiguousRun(pos, runFrames);
#else
        runFrames = mCapacity - pos;
        return mBuffer.data() + static_cast<size_t>(pos) * 2;
#endif
    }

    // ---- single-level undo ----

    bool saveUndo() {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        // Give the pool headroom so copy-on-write materialisation during the
        // overdub never has to allocate on the audio thread.
        mPool.prefill(static_cast<size_t>(pageCountFor(mCapacity)));
        mChunked.snapshotForUndo();
        return true;
#else
        try {
            mUndoBuffer.resize(mBuffer.size());
            std::copy(mBuffer.begin(), mBuffer.end(), mUndoBuffer.begin());
            mHasUndo = true;
            return true;
        } catch (...) { return false; }
#endif
    }

    bool restoreUndo() {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        if (!mChunked.hasUndo()) return false;
        mChunked.restoreUndo();
        mCapacity = mChunked.capacityFrames();
        return true;
#else
        if (!mHasUndo) return false;
        if (mUndoBuffer.size() != mBuffer.size()) return false;
        std::copy(mUndoBuffer.begin(), mUndoBuffer.end(), mBuffer.begin());
        mHasUndo = false;
        return true;
#endif
    }

    bool hasUndo() const {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        return mChunked.hasUndo();
#else
        return mHasUndo;
#endif
    }

#ifndef WM_LOOPER_CHUNKED_BUFFER
    /** Dense-only raw contiguous pointer (offline exporter; retires in F2.3). */
    const float* denseData() const { return mBuffer.data(); }
#endif

private:
    static int pageCountFor(int frames) {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        return (frames + ChunkedAudioBuffer::kChunkFrames - 1)
                   >> ChunkedAudioBuffer::kChunkShift;
#else
        (void)frames; return 0;
#endif
    }

    int mCapacity = 0;

#ifdef WM_LOOPER_CHUNKED_BUFFER
    ChunkPool mPool;
    ChunkedAudioBuffer mChunked;
#else
    std::vector<float> mBuffer;
    std::vector<float> mUndoBuffer;
    bool mHasUndo = false;
#endif
};

}  // namespace wm
