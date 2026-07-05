#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace wm {

/**
 * @struct Chunk
 * @brief Fixed-size block of stereo audio, the allocation unit of the paged
 *        looper buffer (plan §3.1). 32768 frames × 2ch × 4B = 256 KB.
 *
 * `poolNext` is used only while the chunk sits in a ChunkPool free-list; the
 * audio payload lives in `data` (interleaved L/R).
 */
struct Chunk {
    static constexpr int    kFrames  = 32768;          // ~0.68 s @ 48k
    static constexpr size_t kSamples = static_cast<size_t>(kFrames) * 2;
    static constexpr size_t kBytes   = kSamples * sizeof(float);  // 256 KB

    Chunk* poolNext = nullptr;                          // free-list link (pool only)
    alignas(64) float data[kSamples];
};

/**
 * @class ChunkPool
 * @brief Lock-free free-list of fixed 256 KB audio chunks.
 *
 * Ownership model (plan §3.1):
 * - UI/IO thread PRE-FILLS the pool (prefill) and RELEASES chunks back to it.
 * - The audio (RT) thread only ACQUIRES chunks (materialising a page on write)
 *   and never allocates: acquire() is wait-free and returns nullptr when the
 *   pool is empty (the caller then drops the frame and counts it).
 *
 * Implementation: a Treiber stack. Correctness note — the audio thread is the
 * ONLY consumer (single popper), so the classic ABA hazard on pop cannot occur:
 * a chunk being popped is never simultaneously released (only the popper owns
 * it mid-pop), so its `poolNext` is stable. Multiple producers (UI/IO) push via
 * a CAS loop. The pool OWNS every chunk it allocates and deletes them all in the
 * destructor; the owner registry is touched only on non-RT threads.
 */
class ChunkPool {
public:
    ChunkPool() = default;
    ~ChunkPool() {
        std::lock_guard<std::mutex> lk(mOwnMutex);
        for (Chunk* c : mOwned) delete c;
        mOwned.clear();
        mFreeHead.store(nullptr, std::memory_order_relaxed);
    }

    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;

    /**
     * @brief Ensure at least `target` free chunks are available. UI/IO thread.
     *        Allocates the shortfall and pushes them onto the free-list.
     */
    void prefill(size_t target) {
        while (mFreeCount.load(std::memory_order_relaxed) < target) {
            Chunk* c = new (std::nothrow) Chunk();
            if (!c) return;  // out of memory — leave the pool as large as we got
            {
                std::lock_guard<std::mutex> lk(mOwnMutex);
                mOwned.push_back(c);
            }
            release(c);
        }
    }

    /** RT-safe: take a free chunk, or nullptr if the pool is empty. */
    Chunk* acquire() noexcept {
        Chunk* head = mFreeHead.load(std::memory_order_acquire);
        while (head) {
            Chunk* next = head->poolNext;
            if (mFreeHead.compare_exchange_weak(head, next,
                    std::memory_order_acquire, std::memory_order_acquire)) {
                head->poolNext = nullptr;
                mFreeCount.fetch_sub(1, std::memory_order_relaxed);
                return head;
            }
            // head reloaded by compare_exchange_weak on failure; retry.
        }
        return nullptr;
    }

    /** Return a chunk to the free-list. Non-RT (UI/IO) or RT — lock-free push. */
    void release(Chunk* c) noexcept {
        if (!c) return;
        Chunk* head = mFreeHead.load(std::memory_order_relaxed);
        do {
            c->poolNext = head;
        } while (!mFreeHead.compare_exchange_weak(head, c,
                    std::memory_order_release, std::memory_order_relaxed));
        mFreeCount.fetch_add(1, std::memory_order_relaxed);
    }

    /** Approximate free-chunk count (telemetry / tests). */
    size_t freeCount() const noexcept { return mFreeCount.load(std::memory_order_relaxed); }

    /** Total chunks owned by the pool (free + in use). */
    size_t totalChunks() const {
        std::lock_guard<std::mutex> lk(mOwnMutex);
        return mOwned.size();
    }

private:
    std::atomic<Chunk*> mFreeHead{nullptr};
    std::atomic<size_t> mFreeCount{0};

    mutable std::mutex mOwnMutex;   // non-RT only
    std::vector<Chunk*> mOwned;     // every chunk ever allocated (for teardown)
};

}  // namespace wm
