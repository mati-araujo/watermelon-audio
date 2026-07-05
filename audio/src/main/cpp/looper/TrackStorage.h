#pragma once

// ============================================================================
// TrackStorage — the audio-sample storage backing a single looper track.
//
// Two interchangeable implementations selected at compile time (plan §3.1):
//   - default:                  paged ChunkedAudioBuffer (silence = no memory,
//                               O(pages) trim/pad, copy-on-write undo, budget
//                               bounds pool RAM which trim hands back to the OS).
//   - -D WM_LOOPER_DENSE_BUFFER: legacy dense std::vector<float> escape hatch.
//
// TrackBuffer talks only to this uniform API, so the RT/analysis code has no
// #ifdefs. Everything internally keys off WM_LOOPER_CHUNKED_BUFFER, which we
// DERIVE below (defined unless the dense opt-out is requested) so no existing
// #ifdef site had to flip when the paged backend became the default.
// ============================================================================

// Backend selection: paged is the default; WM_LOOPER_DENSE_BUFFER opts out.
#if !defined(WM_LOOPER_DENSE_BUFFER) && !defined(WM_LOOPER_CHUNKED_BUFFER)
#define WM_LOOPER_CHUNKED_BUFFER 1
#endif

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
        // allocates while recording within [0, frames). This RAM is now honestly
        // counted by reservedBytes() (the memory budget bounds it) and handed back
        // to the OS by shrinkToContent() once a shorter take is trimmed.
        mPool.prefill(static_cast<size_t>(pageCountFor(frames)));
        mChunked.setPool(&mPool);
        mChunked.reset(frames);
#else
        mBuffer.assign(static_cast<size_t>(frames) * 2, 0.0f);
#endif
        return static_cast<size_t>(frames) * 2 * sizeof(float);
    }

    /**
     * @brief Return the pool's unused slack to the OS, keeping only the in-use
     *        chunks plus a small headroom. UI/IO thread; caller guarantees RT is
     *        quiesced (post-finalize / not recording). No-op when dense. This is
     *        what makes a 60 s free take that recorded only 10 s cost ~4 MB, not 23.
     */
    void shrinkToContent() {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        mPool.trimTo(mChunked.ownedChunks() + kPoolRetainChunks);
#endif
    }

    /**
     * @brief Real RAM this track reserves: pool-owned chunks (chunked) or the
     *        buffer + undo capacity (dense). This — not allocatedBytes(), which
     *        counts only materialised pages — is what the memory budget must bound.
     */
    size_t reservedBytes() const {
#ifdef WM_LOOPER_CHUNKED_BUFFER
        return mPool.totalChunks() * Chunk::kBytes;
#else
        return mBuffer.capacity() * sizeof(float) + mUndoBuffer.capacity() * sizeof(float);
#endif
    }

    /** Release all memory / return chunks to the pool; go empty. */
    void release() {
        mCapacity = 0;
#ifdef WM_LOOPER_CHUNKED_BUFFER
        mChunked.clear();
        mPool.trimTo(kPoolRetainChunks);   // hand the freed chunks back to the OS
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
        shrinkToContent();                 // reclaim the pre-reserved free-take slack
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
        // COW headroom so materialisation during the overdub never allocates on the
        // audio thread. An overdub copies at most every currently in-use page, so
        // reserve that many free chunks — bounded by real content, not full capacity.
        mPool.prefill(mPool.freeCount() + mChunked.ownedChunks());
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
    // Free chunks the pool keeps after a shrink, so a subsequent short write does
    // not immediately have to grow the pool again.
    static constexpr size_t kPoolRetainChunks = 2;

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
