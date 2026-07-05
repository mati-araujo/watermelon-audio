// ============================================================================
// test_chunked_buffer — ChunkedAudioBuffer + ChunkPool (plan §3.1, F2.1).
//
// Written BEFORE integrating chunks into TrackBuffer, per the plan: this is the
// data-structure contract the integration must preserve. Single-threaded /
// host-side; the concurrent audio-vs-UI stress is the separate TSan harness.
// ============================================================================
#include <gtest/gtest.h>
#include "ChunkedAudioBuffer.h"
#include "ChunkPool.h"

#include <vector>

using wm::Chunk;
using wm::ChunkPool;
using wm::ChunkedAudioBuffer;

namespace {
constexpr int kCF = ChunkedAudioBuffer::kChunkFrames;   // 32768
constexpr size_t kCB = ChunkedAudioBuffer::kChunkBytes;

// Frame position at the start of page `p` plus `off`.
int at(int page, int off = 0) { return page * kCF + off; }
}  // namespace

// ---- silence ----

TEST(ChunkedBuffer, SilentBufferReadsZeroAndCostsNothing) {
    ChunkPool pool; pool.prefill(8);
    ChunkedAudioBuffer buf; buf.setPool(&pool);
    buf.reset(100000);                       // 4 pages

    EXPECT_EQ(buf.pageCount(), 4);
    EXPECT_EQ(buf.capacityFrames(), 100000);
    EXPECT_EQ(buf.allocatedBytes(), 0u);

    for (int p : {at(0), at(1, 5), at(2), at(3, 100)}) {
        float l = 9.0f, r = 9.0f;
        buf.readFrame(p, l, r);
        EXPECT_FLOAT_EQ(l, 0.0f);
        EXPECT_FLOAT_EQ(r, 0.0f);
        EXPECT_FLOAT_EQ(buf.sampleAt(p, 0), 0.0f);
    }
    // Out-of-range reads are silent, not crashes.
    float l, r; buf.readFrame(-1, l, r); EXPECT_FLOAT_EQ(l, 0.0f);
    buf.readFrame(999999, l, r); EXPECT_FLOAT_EQ(l, 0.0f);
}

// ---- write materialises exactly one page ----

TEST(ChunkedBuffer, WriteMaterialisesSinglePage) {
    ChunkPool pool; pool.prefill(8);
    const size_t free0 = pool.freeCount();
    ChunkedAudioBuffer buf; buf.setPool(&pool);
    buf.reset(100000);                       // 4 pages

    EXPECT_TRUE(buf.writeFrame(at(1, 7), 0.5f, -0.5f));   // page 1 only
    EXPECT_EQ(buf.allocatedBytes(), kCB);                 // exactly one chunk
    EXPECT_EQ(pool.freeCount(), free0 - 1);

    float l, r;
    buf.readFrame(at(1, 7), l, r);
    EXPECT_FLOAT_EQ(l, 0.5f);
    EXPECT_FLOAT_EQ(r, -0.5f);
    // Neighbouring pages stay silent.
    buf.readFrame(at(0), l, r); EXPECT_FLOAT_EQ(l, 0.0f);
    buf.readFrame(at(2), l, r); EXPECT_FLOAT_EQ(l, 0.0f);
}

// ---- pool exhaustion drops frames, no alloc, no crash ----

TEST(ChunkedBuffer, WriteDropsWhenPoolExhausted) {
    ChunkPool pool; pool.prefill(2);         // only 2 chunks available
    ChunkedAudioBuffer buf; buf.setPool(&pool);
    buf.reset(at(6, 1));                      // 7 pages

    EXPECT_TRUE(buf.writeFrame(at(0), 1.0f, 1.0f));
    EXPECT_TRUE(buf.writeFrame(at(1), 1.0f, 1.0f));
    // Pool now empty → third distinct page can't be materialised.
    EXPECT_FALSE(buf.writeFrame(at(2), 1.0f, 1.0f));
    EXPECT_EQ(buf.allocatedBytes(), 2u * kCB);
    EXPECT_EQ(pool.freeCount(), 0u);

    // The dropped page is still silent.
    float l, r; buf.readFrame(at(2), l, r);
    EXPECT_FLOAT_EQ(l, 0.0f);
    // Writing again into an already-materialised page still works.
    EXPECT_TRUE(buf.writeFrame(at(0, 1), 2.0f, 2.0f));
}

// ---- trim: O(pages), stable pointers, returns pages to pool ----

TEST(ChunkedBuffer, TrimReturnsPagesWithStablePointers) {
    ChunkPool pool; pool.prefill(8);
    ChunkedAudioBuffer buf; buf.setPool(&pool);
    buf.reset(7 * kCF);                       // 7 full pages
    for (int p = 0; p < 7; ++p) buf.writeFrame(at(p, 3), static_cast<float>(p + 1), 0.0f);
    EXPECT_EQ(buf.allocatedBytes(), 7u * kCB);

    const Chunk* keep0 = buf.pageAt(0);
    const Chunk* keep3 = buf.pageAt(3);
    const size_t freeBefore = pool.freeCount();

    buf.trim(4 * kCF);                         // → 4 pages
    EXPECT_EQ(buf.pageCount(), 4);
    EXPECT_EQ(buf.capacityFrames(), 4 * kCF);
    EXPECT_EQ(buf.allocatedBytes(), 4u * kCB);
    EXPECT_EQ(pool.freeCount(), freeBefore + 3);   // 3 pages returned

    // Kept pages are the SAME chunks (no content moved) with content intact.
    EXPECT_EQ(buf.pageAt(0), keep0);
    EXPECT_EQ(buf.pageAt(3), keep3);
    float l, r; buf.readFrame(at(3, 3), l, r);
    EXPECT_FLOAT_EQ(l, 4.0f);
}

// ---- pad: adds silent pages, O(1), no alloc ----

TEST(ChunkedBuffer, PadAddsSilentPagesNoAlloc) {
    ChunkPool pool; pool.prefill(8);
    ChunkedAudioBuffer buf; buf.setPool(&pool);
    buf.reset(40000);                         // 2 pages
    buf.writeFrame(at(0, 9), 0.7f, 0.7f);
    EXPECT_EQ(buf.allocatedBytes(), kCB);

    buf.pad(200000);                          // → 7 pages
    EXPECT_EQ(buf.pageCount(), 7);
    EXPECT_EQ(buf.capacityFrames(), 200000);
    EXPECT_EQ(buf.allocatedBytes(), kCB);     // no new chunks

    float l, r;
    buf.readFrame(at(5), l, r); EXPECT_FLOAT_EQ(l, 0.0f);   // padded → silent
    buf.readFrame(at(0, 9), l, r); EXPECT_FLOAT_EQ(l, 0.7f); // original intact
}

// ---- copy-on-write undo ----

TEST(ChunkedBuffer, CowUndoRestoresBitExactAndFreesCopies) {
    ChunkPool pool; pool.prefill(16);
    ChunkedAudioBuffer buf; buf.setPool(&pool);
    buf.reset(at(6, 1));                      // 7 pages

    // Base take: distinct content in pages 0, 1, 2.
    buf.writeFrame(at(0, 1), 0.10f, -0.10f);
    buf.writeFrame(at(1, 2), 0.20f, -0.20f);
    buf.writeFrame(at(2, 3), 0.30f, -0.30f);
    ASSERT_EQ(buf.ownedChunks(), 3u);

    buf.snapshotForUndo();
    EXPECT_TRUE(buf.hasUndo());
    EXPECT_EQ(buf.allocatedBytes(), 3u * kCB);   // snapshot is shallow

    // Overdub: modify page 0 and page 2 (K=2 shared pages → 2 COW copies),
    // and write a brand-new page 3 (was silent → +1).
    buf.writeFrame(at(0, 1), 0.99f, 0.99f);
    buf.writeFrame(at(2, 3), 0.88f, 0.88f);
    buf.writeFrame(at(3, 4), 0.77f, 0.77f);
    EXPECT_EQ(buf.ownedChunks(), 6u);            // 3 base + 2 COW + 1 new

    float l, r;
    buf.readFrame(at(0, 1), l, r); EXPECT_FLOAT_EQ(l, 0.99f);
    buf.readFrame(at(3, 4), l, r); EXPECT_FLOAT_EQ(l, 0.77f);

    const size_t freeBeforeRestore = pool.freeCount();
    buf.restoreUndo();
    EXPECT_FALSE(buf.hasUndo());
    EXPECT_EQ(buf.ownedChunks(), 3u);            // copies + new page freed
    EXPECT_EQ(pool.freeCount(), freeBeforeRestore + 3);

    // Content is bit-exact to the pre-overdub take.
    buf.readFrame(at(0, 1), l, r); EXPECT_FLOAT_EQ(l, 0.10f); EXPECT_FLOAT_EQ(r, -0.10f);
    buf.readFrame(at(2, 3), l, r); EXPECT_FLOAT_EQ(l, 0.30f);
    buf.readFrame(at(3, 4), l, r); EXPECT_FLOAT_EQ(l, 0.0f);  // new page rolled back to silence
}

TEST(ChunkedBuffer, DiscardUndoCommitsOverdubAndFreesOriginals) {
    ChunkPool pool; pool.prefill(16);
    ChunkedAudioBuffer buf; buf.setPool(&pool);
    buf.reset(at(3, 1));                      // 4 pages

    buf.writeFrame(at(0, 1), 0.10f, 0.10f);
    buf.writeFrame(at(1, 1), 0.20f, 0.20f);
    ASSERT_EQ(buf.ownedChunks(), 2u);

    buf.snapshotForUndo();
    buf.writeFrame(at(0, 1), 0.55f, 0.55f);  // COW copy of page 0
    EXPECT_EQ(buf.ownedChunks(), 3u);

    const size_t freeBefore = pool.freeCount();
    buf.discardUndo();                        // commit → free the superseded original
    EXPECT_FALSE(buf.hasUndo());
    EXPECT_EQ(buf.ownedChunks(), 2u);
    EXPECT_EQ(pool.freeCount(), freeBefore + 1);

    // Committed content = the overdub.
    float l, r; buf.readFrame(at(0, 1), l, r);
    EXPECT_FLOAT_EQ(l, 0.55f);
}

// ---- accounting stays exact through a mixed sequence ----

TEST(ChunkedBuffer, AllocatedBytesTracksLiveChunks) {
    ChunkPool pool; pool.prefill(16);
    ChunkedAudioBuffer buf; buf.setPool(&pool);

    buf.reset(at(5, 1));                      // 6 pages
    EXPECT_EQ(buf.allocatedBytes(), 0u);
    for (int p = 0; p < 4; ++p) buf.writeFrame(at(p), 1.0f, 1.0f);
    EXPECT_EQ(buf.allocatedBytes(), 4u * kCB);

    buf.trim(at(1, 1));                       // 2 pages kept
    EXPECT_EQ(buf.allocatedBytes(), 2u * kCB);

    buf.clear();
    EXPECT_EQ(buf.allocatedBytes(), 0u);
    EXPECT_EQ(buf.pageCount(), 0);
    // Everything returned to the pool.
    EXPECT_EQ(pool.freeCount(), pool.totalChunks());
}
