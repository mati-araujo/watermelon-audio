#pragma once

#include "ChunkPool.h"
#include <atomic>
#include <cstring>
#include <vector>

namespace wm {

/**
 * @class ChunkedAudioBuffer
 * @brief Paged stereo audio buffer (plan §3.1). Replaces the dense
 *        std::vector<float> so silence costs no memory and trim/pad/undo are
 *        O(pages) with no big contiguous reallocs.
 *
 * Layout:
 *   - A page table (std::vector<Chunk*>), indexed by frame >> kChunkShift.
 *   - nullptr entry  = silent page (no chunk, reads emit zeros).
 *   - non-null entry = a 256 KB Chunk taken from the ChunkPool.
 *
 * Threading:
 *   - readFrame/sampleAt/writeFrame run on the audio thread. writeFrame only
 *     ever mutates existing page-table SLOTS (materialising a page); it never
 *     resizes the table, so it does not race with concurrent reads on the same
 *     thread. It takes chunks from the pool (wait-free) and never allocates.
 *   - reset/trim/pad/clear/snapshot/restore/materializeRegion run on the UI/IO
 *     thread. The owner (TrackBuffer) serialises these against the audio thread
 *     with its existing mPlaying=false + waitForRenderIdle() contract before
 *     touching the table.
 *
 * Copy-on-write undo:
 *   snapshotForUndo() shallow-copies the page table and marks every current
 *   page shared. The first write to a shared, non-silent page materialises a
 *   private copy (so the snapshot keeps the pre-overdub content). restoreUndo()
 *   swaps back and frees the copies; discardUndo() commits and frees the
 *   superseded originals. Extra memory during an overdub = only the pages
 *   actually touched.
 *
 * Accounting: allocatedBytes() is exact — one running counter tracks chunks
 * owned (bumped on every pool acquire, dropped on every release).
 */
class ChunkedAudioBuffer {
public:
    static constexpr int    kChunkFrames = Chunk::kFrames;   // 32768
    static constexpr int    kChunkShift  = 15;               // 2^15 == 32768
    static constexpr int    kChunkMask   = kChunkFrames - 1;
    static constexpr size_t kChunkBytes  = Chunk::kBytes;

    static_assert((1 << kChunkShift) == kChunkFrames, "kChunkShift must match kChunkFrames");

    ChunkedAudioBuffer() = default;
    ~ChunkedAudioBuffer() { clear(); }

    ChunkedAudioBuffer(const ChunkedAudioBuffer&) = delete;
    ChunkedAudioBuffer& operator=(const ChunkedAudioBuffer&) = delete;

    /** Bind the chunk pool. Call once before use (UI thread). */
    void setPool(ChunkPool* pool) { mPool = pool; }
    ChunkPool* pool() const { return mPool; }

    /**
     * @brief (Re)initialise to `capacityFrames` of silence. Returns any existing
     *        chunks (active + undo) to the pool. UI/IO thread.
     */
    void reset(int capacityFrames) {
        releaseAll();
        mCapacityFrames = (capacityFrames > 0) ? capacityFrames : 0;
        mPages.assign(pageCountFor(mCapacityFrames), nullptr);
    }

    int capacityFrames() const { return mCapacityFrames; }
    int pageCount() const { return static_cast<int>(mPages.size()); }
    bool hasUndo() const { return mHasUndo; }

    /** Exact allocated size = owned chunks × chunk bytes. */
    size_t allocatedBytes() const { return mOwnedChunks * kChunkBytes; }
    size_t ownedChunks() const { return mOwnedChunks; }

    // ---------------- Audio thread: read ----------------

    /** Read one stereo frame; silent pages (and out-of-range) yield zeros. */
    void readFrame(int pos, float& l, float& r) const {
        if (pos < 0 || pos >= mCapacityFrames) { l = 0.0f; r = 0.0f; return; }
        const Chunk* c = mPages[static_cast<size_t>(pos) >> kChunkShift];
        if (!c) { l = 0.0f; r = 0.0f; return; }
        const size_t o = static_cast<size_t>(pos & kChunkMask) * 2;
        l = c->data[o];
        r = c->data[o + 1];
    }

    /**
     * @brief Pointer to a contiguous run of interleaved samples starting at
     *        `pos`, and how many frames are contiguous from there (bounded by the
     *        page end and the capacity). Returns nullptr for a silent run (the
     *        caller emits `runFrames` zeros). Lets mixInto branch per page, not
     *        per sample. `runFrames` is 0 only when `pos` is out of range.
     */
    const float* contiguousRun(int pos, int& runFrames) const {
        if (pos < 0 || pos >= mCapacityFrames) { runFrames = 0; return nullptr; }
        const int off = pos & kChunkMask;
        int avail = kChunkFrames - off;
        if (pos + avail > mCapacityFrames) avail = mCapacityFrames - pos;
        runFrames = avail;
        const Chunk* c = mPages[static_cast<size_t>(pos) >> kChunkShift];
        return c ? (c->data + static_cast<size_t>(off) * 2) : nullptr;
    }

    /** Single-channel sample (for interpolation). Silent/out-of-range → 0. */
    float sampleAt(int pos, int channel) const {
        if (pos < 0 || pos >= mCapacityFrames) return 0.0f;
        const Chunk* c = mPages[static_cast<size_t>(pos) >> kChunkShift];
        if (!c) return 0.0f;
        return c->data[static_cast<size_t>(pos & kChunkMask) * 2 + channel];
    }

    // ---------------- Audio thread: write ----------------

    /**
     * @brief Write one stereo frame, materialising the page from the pool if it
     *        is silent (or copying it if shared with an undo snapshot). RT-safe.
     * @return false if `pos` is out of range OR the pool was empty (frame
     *         dropped — the caller counts it).
     */
    bool writeFrame(int pos, float l, float r) {
        if (pos < 0 || pos >= mCapacityFrames) return false;
        const int page = static_cast<int>(static_cast<size_t>(pos) >> kChunkShift);
        Chunk* c = pageForWrite(page);
        if (!c) return false;  // pool exhausted
        const size_t o = static_cast<size_t>(pos & kChunkMask) * 2;
        c->data[o]     = l;
        c->data[o + 1] = r;
        return true;
    }

    // ---------------- UI/IO thread: structure ----------------

    /**
     * @brief Truncate to `newFrames`. Pages beyond the new capacity are returned
     *        to the pool. O(pages), no content is moved. Discards any undo.
     */
    void trim(int newFrames) {
        if (newFrames < 0) newFrames = 0;
        if (newFrames >= mCapacityFrames) { mCapacityFrames = newFrames; return; }
        discardUndo();
        const int newPages = pageCountFor(newFrames);
        for (int i = newPages; i < static_cast<int>(mPages.size()); ++i) {
            releaseChunk(mPages[i]);
        }
        mPages.resize(static_cast<size_t>(newPages), nullptr);
        mCapacityFrames = newFrames;
    }

    /**
     * @brief Grow to `newFrames`, appending SILENT pages (nullptr). O(1) per
     *        added page, no allocation. Discards any undo.
     */
    void pad(int newFrames) {
        if (newFrames <= mCapacityFrames) { if (newFrames > mCapacityFrames) mCapacityFrames = newFrames; return; }
        discardUndo();
        mPages.resize(static_cast<size_t>(pageCountFor(newFrames)), nullptr);
        mCapacityFrames = newFrames;
    }

    /** Return all chunks (active + undo) to the pool and go empty. */
    void clear() {
        releaseAll();
        mCapacityFrames = 0;
        mPages.clear();
    }

    // ---------------- Copy-on-write undo ----------------

    /**
     * @brief Snapshot the current content for a single-level undo. Cheap:
     *        shallow-copies page pointers and marks them shared (copy-on-write).
     *        UI thread, before an overdub.
     */
    void snapshotForUndo() {
        discardUndo();
        mUndoPages = mPages;                 // shallow: shares chunk pointers
        mUndoCapacity = mCapacityFrames;
        mCowShared.assign(mPages.size(), true);
        mHasUndo = true;
    }

    /**
     * @brief Pre-materialise the copy-on-write pages overlapping [startFrame,
     *        endFrame) on the UI thread, so subsequent overdub writes on the
     *        audio thread never have to allocate. No-op without an active undo.
     */
    void materializeRegion(int startFrame, int endFrame) {
        if (!mHasUndo || !mPool) return;
        if (startFrame < 0) startFrame = 0;
        if (endFrame > mCapacityFrames) endFrame = mCapacityFrames;
        if (endFrame <= startFrame) return;
        const int firstPage = startFrame >> kChunkShift;
        const int lastPage  = (endFrame - 1) >> kChunkShift;
        for (int p = firstPage; p <= lastPage && p < static_cast<int>(mPages.size()); ++p) {
            (void)materializeCow(p);
        }
    }

    /** Roll back to the snapshot, freeing pages materialised since. UI thread. */
    void restoreUndo() {
        if (!mHasUndo) return;
        const size_t n = mPages.size();
        for (size_t i = 0; i < n; ++i) {
            Chunk* active = mPages[i];
            Chunk* snap = (i < mUndoPages.size()) ? mUndoPages[i] : nullptr;
            if (active != snap) releaseChunk(active);  // materialised copy / new page
        }
        mPages = mUndoPages;
        mCapacityFrames = mUndoCapacity;
        mUndoPages.clear();
        mCowShared.clear();
        mHasUndo = false;
    }

    /** Commit the overdub, freeing the snapshot's superseded originals. */
    void discardUndo() {
        if (!mHasUndo) return;
        const size_t n = mUndoPages.size();
        for (size_t i = 0; i < n; ++i) {
            Chunk* snap = mUndoPages[i];
            Chunk* active = (i < mPages.size()) ? mPages[i] : nullptr;
            if (snap != active) releaseChunk(snap);  // superseded original
        }
        mUndoPages.clear();
        mCowShared.clear();
        mHasUndo = false;
    }

    // ---------------- Export iteration (F2.3) ----------------

    /** Direct page access for the offline exporter (reads under ExportGuard). */
    const Chunk* pageAt(int page) const {
        return (page >= 0 && page < static_cast<int>(mPages.size())) ? mPages[page] : nullptr;
    }

private:
    static int pageCountFor(int frames) {
        return (frames + kChunkFrames - 1) >> kChunkShift;
    }

    // Return the writable chunk for `page`, materialising or COW-copying as
    // needed. nullptr only when the pool is exhausted.
    Chunk* pageForWrite(int page) {
        Chunk* c = mPages[page];
        if (c) {
            // Copy-on-write if this page is shared with the undo snapshot.
            if (mHasUndo && page < static_cast<int>(mCowShared.size()) && mCowShared[page]) {
                return materializeCow(page);
            }
            return c;
        }
        // Silent page → materialise a zeroed chunk.
        if (!mPool) return nullptr;
        Chunk* fresh = mPool->acquire();
        if (!fresh) return nullptr;
        std::memset(fresh->data, 0, kChunkBytes);
        mPages[page] = fresh;
        ++mOwnedChunks;
        if (mHasUndo && page < static_cast<int>(mCowShared.size())) mCowShared[page] = false;
        return fresh;
    }

    // Copy a shared page so the active table owns a private mutable copy.
    // Returns nullptr (leaving the page shared) if the pool is exhausted.
    Chunk* materializeCow(int page) {
        Chunk* orig = mPages[page];
        if (!orig) { if (page < static_cast<int>(mCowShared.size())) mCowShared[page] = false; return nullptr; }
        if (!mPool) return nullptr;
        Chunk* copy = mPool->acquire();
        if (!copy) return nullptr;  // can't COW → drop (page stays shared)
        std::memcpy(copy->data, orig->data, kChunkBytes);
        mPages[page] = copy;
        ++mOwnedChunks;
        if (page < static_cast<int>(mCowShared.size())) mCowShared[page] = false;
        return copy;
    }

    void releaseChunk(Chunk* c) {
        if (!c || !mPool) return;
        mPool->release(c);
        --mOwnedChunks;
    }

    // Free every chunk owned across active + undo tables (each once).
    void releaseAll() {
        // discardUndo frees undo originals not shared with active; then active
        // holds the union of live chunks, which we free below.
        discardUndo();
        for (Chunk*& c : mPages) { releaseChunk(c); c = nullptr; }
    }

    ChunkPool* mPool = nullptr;
    int mCapacityFrames = 0;
    std::vector<Chunk*> mPages;       // active page table (nullptr = silence)

    // Undo (copy-on-write) state.
    std::vector<Chunk*> mUndoPages;
    std::vector<bool>   mCowShared;   // per active page: shared with undo snapshot
    int  mUndoCapacity = 0;
    bool mHasUndo = false;

    size_t mOwnedChunks = 0;          // exact count for allocatedBytes()
};

}  // namespace wm
