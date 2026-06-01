#pragma once

#include "../dsp/LockFreeRingBuffer.h"

#include <algorithm>
#include <array>
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
 * The audio/USB hot paths only pay one atomic slot load before using the
 * underlying LockFreeRingBuffer. Resizing happens off the per-transfer path:
 * a new inactive slot is allocated, prepared, optionally prefilled, and then
 * published with a release-store. Old buffers are kept alive until reset()
 * or destruction so any in-flight reader/writer that already grabbed a raw
 * pointer can finish without a use-after-free.
 */
class ResizableRingBuffer {
public:
    ResizableRingBuffer() = default;

    ResizableRingBuffer(const ResizableRingBuffer&) = delete;
    ResizableRingBuffer& operator=(const ResizableRingBuffer&) = delete;

    template <typename PrepareFn>
    void reset(size_t capacity, PrepareFn&& prepare) {
        mSlots[0] = std::make_unique<LockFreeRingBuffer>(capacity);
        std::forward<PrepareFn>(prepare)(*mSlots[0]);
        mSlots[1].reset();
        mRetired.clear();
        mActiveSlot.store(0, std::memory_order_release);
    }

    template <typename PrepareFn>
    void resize(size_t capacity,
                PrepareFn&& prepare,
                const float* prefillData = nullptr,
                size_t prefillSamples = 0) {
        const int current = mActiveSlot.load(std::memory_order_acquire);
        const int next = current ^ 1;

        if (mSlots[next]) {
            mRetired.push_back(std::move(mSlots[next]));
        }

        auto newBuffer = std::make_unique<LockFreeRingBuffer>(capacity);
        std::forward<PrepareFn>(prepare)(*newBuffer);

        if (prefillData && prefillSamples > 0 && newBuffer->capacity() > 0) {
            const size_t safeSamples = std::min(prefillSamples, newBuffer->capacity() - 1);
            if (safeSamples > 0) {
                newBuffer->write(prefillData, safeSamples);
            }
        }

        mSlots[next] = std::move(newBuffer);
        mActiveSlot.store(next, std::memory_order_release);
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
        return mSlots[mActiveSlot.load(std::memory_order_acquire)].get();
    }

    const LockFreeRingBuffer* active() const {
        return mSlots[mActiveSlot.load(std::memory_order_acquire)].get();
    }

    std::array<std::unique_ptr<LockFreeRingBuffer>, 2> mSlots;
    std::atomic<int> mActiveSlot{0};
    std::vector<std::unique_ptr<LockFreeRingBuffer>> mRetired;
};

} // namespace usb
} // namespace watermelon_audio
