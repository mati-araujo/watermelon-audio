#pragma once

#include "../dsp/LockFreeRingBuffer.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace watermelon_audio {
namespace usb {

/**
 * SPSC ring buffer wrapper that can swap capacity safely while streaming.
 *
 * The audio/USB hot paths only pay one atomic pointer load before using the
 * underlying LockFreeRingBuffer. Resizing happens off the per-transfer path:
 * a new buffer is allocated, prepared, optionally prefilled, and then
 * published with a release-store. Old buffers are kept alive until reset()
 * or destruction so any in-flight reader/writer that already grabbed a raw
 * pointer can finish without a use-after-free.
 *
 * ## What readers may touch
 * Exactly one thing: [mActive], an atomic raw pointer. Ownership lives in
 * [mOwned], which only the resizing thread ever touches.
 *
 * That split is the whole point. An earlier version published an *index* into
 * an array of `unique_ptr` slots, and resize() moved the displaced slot into a
 * retirement list. Moving a `unique_ptr` writes to the source — it nulls it —
 * so a reader that had loaded the index before the previous publish, and was
 * still evaluating `mSlots[i].get()`, raced with that write. The pointee did
 * survive (that part of the design worked), but the smart pointer *object*
 * holding it was being mutated under the reader. TSan caught it in
 * ResizableRingBufferTest.StressWriterReaderAndRepeatedResize once the thread
 * job was fixed enough to actually run. Publishing a raw pointer removes the
 * shared mutable object entirely, so a stale load now yields a pointer that is
 * still valid rather than one being torn to null.
 */
class ResizableRingBuffer {
public:
    ResizableRingBuffer() = default;

    ResizableRingBuffer(const ResizableRingBuffer&) = delete;
    ResizableRingBuffer& operator=(const ResizableRingBuffer&) = delete;

    /**
     * Re-initializes the buffer, dropping every previously allocated one.
     *
     * Unlike [resize] this is NOT safe against a concurrent reader/writer: it
     * frees the buffers they may still hold. Call it while the stream is
     * stopped. The old buffers are released only after the new one is
     * published, which keeps the unavoidable window as small as possible.
     */
    template <typename PrepareFn>
    void reset(size_t capacity, PrepareFn&& prepare) {
        auto buffer = std::make_unique<LockFreeRingBuffer>(capacity);
        std::forward<PrepareFn>(prepare)(*buffer);

        LockFreeRingBuffer* published = buffer.get();
        std::vector<std::unique_ptr<LockFreeRingBuffer>> discarded;
        discarded.swap(mOwned);
        mOwned.push_back(std::move(buffer));
        mActive.store(published, std::memory_order_release);
        // `discarded` frees the previous buffers here, after the publish.
    }

    /**
     * Swaps in a buffer of a different capacity while the stream runs.
     *
     * Safe against a concurrent reader/writer: the previous buffer is retained
     * (see [mOwned]) rather than freed, so a hot path that already loaded it
     * finishes against valid memory. Retained buffers are released on [reset]
     * or destruction, so a long-running stream that resizes repeatedly does
     * hold them all — the same accumulation the retirement list had before.
     */
    template <typename PrepareFn>
    void resize(size_t capacity,
                PrepareFn&& prepare,
                const float* prefillData = nullptr,
                size_t prefillSamples = 0) {
        auto newBuffer = std::make_unique<LockFreeRingBuffer>(capacity);
        std::forward<PrepareFn>(prepare)(*newBuffer);

        if (prefillData && prefillSamples > 0 && newBuffer->capacity() > 0) {
            const size_t safeSamples = std::min(prefillSamples, newBuffer->capacity() - 1);
            if (safeSamples > 0) {
                newBuffer->write(prefillData, safeSamples);
            }
        }

        // Take ownership before publishing: mOwned is the resizing thread's
        // alone, so growing it cannot be observed by anyone else, and the
        // pointer is only reachable once the release-store below lands.
        LockFreeRingBuffer* published = newBuffer.get();
        mOwned.push_back(std::move(newBuffer));
        mActive.store(published, std::memory_order_release);
    }

    bool write(const float* samples, size_t numSamples) {
        LockFreeRingBuffer* buffer = active();
        return buffer ? buffer->write(samples, numSamples) : false;
    }

    bool read(float* samples, size_t numSamples) {
        LockFreeRingBuffer* buffer = active();
        return buffer ? buffer->read(samples, numSamples) : false;
    }

    size_t availableToRead() const {
        const LockFreeRingBuffer* buffer = active();
        return buffer ? buffer->availableToRead() : 0;
    }

    size_t availableToWrite() const {
        const LockFreeRingBuffer* buffer = active();
        return buffer ? buffer->availableToWrite() : 0;
    }

    size_t capacity() const {
        const LockFreeRingBuffer* buffer = active();
        return buffer ? buffer->capacity() : 0;
    }

    void clear() {
        if (LockFreeRingBuffer* buffer = active()) {
            buffer->clear();
        }
    }

    float* data() {
        LockFreeRingBuffer* buffer = active();
        return buffer ? buffer->data() : nullptr;
    }

    size_t sizeBytes() const {
        const LockFreeRingBuffer* buffer = active();
        return buffer ? buffer->sizeBytes() : 0;
    }

private:
    LockFreeRingBuffer* active() {
        return mActive.load(std::memory_order_acquire);
    }

    const LockFreeRingBuffer* active() const {
        return mActive.load(std::memory_order_acquire);
    }

    /** The published buffer. The only member a reader/writer may touch. */
    std::atomic<LockFreeRingBuffer*> mActive{nullptr};

    /** Owns every buffer handed out, including retired ones. Resizer-only. */
    std::vector<std::unique_ptr<LockFreeRingBuffer>> mOwned;
};

} // namespace usb
} // namespace watermelon_audio
