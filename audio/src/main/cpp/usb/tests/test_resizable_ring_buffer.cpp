#include "../ResizableRingBuffer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

namespace watermelon_audio::usb {
namespace {

void noPrepare(LockFreeRingBuffer&) {}

TEST(ResizableRingBufferTest, StressWriterReaderAndRepeatedResize) {
    ResizableRingBuffer ring;
    ring.reset(512, noPrepare);

    constexpr size_t kChunkSamples = 16;
    constexpr auto kDuration = std::chrono::seconds(5);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> readFailures{0};
    std::atomic<uint64_t> swaps{0};
    std::atomic<bool> corrupted{false};

    std::thread writer([&]() {
        std::vector<float> chunk(kChunkSamples);
        uint64_t nextValue = 1;

        // A 32-bit float represents every integer exactly only up to 2^24;
        // past that, consecutive integers collide (16777217 rounds to
        // 16777216.0f). Since we encode a strictly-increasing sequence as
        // float and the reader below asserts strict monotonicity, the
        // sequence MUST stay within the float-exact range — otherwise a
        // fast writer that produces >2^24 samples within kDuration trips the
        // check spuriously (machine-speed-dependent, not a ring-buffer bug).
        constexpr uint64_t kMaxExactValue = 1ull << 24;  // 16,777,216

        while (!stop.load(std::memory_order_acquire)) {
            if (nextValue + kChunkSamples >= kMaxExactValue) {
                // Float-exact sequence space exhausted. Stop producing, but
                // leave the reader and resizer to keep stressing concurrent
                // resize for the rest of the window.
                break;
            }

            for (size_t i = 0; i < chunk.size(); ++i) {
                chunk[i] = static_cast<float>(nextValue + i);
            }

            if (ring.write(chunk.data(), chunk.size())) {
                nextValue += chunk.size();
                writes.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::thread reader([&]() {
        std::vector<float> chunk(kChunkSamples);
        uint64_t lastSeen = 0;

        while (!stop.load(std::memory_order_acquire)) {
            if (!ring.read(chunk.data(), chunk.size())) {
                readFailures.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
                continue;
            }

            for (float sample : chunk) {
                const auto value = static_cast<uint64_t>(sample);
                if (sample != static_cast<float>(value) || value <= lastSeen) {
                    corrupted.store(true, std::memory_order_release);
                    stop.store(true, std::memory_order_release);
                    return;
                }
                lastSeen = value;
            }

            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread resizer([&]() {
        std::mt19937 rng(0x574d4155);
        std::uniform_int_distribution<size_t> capacityDist(64, 8192);
        const auto deadline = std::chrono::steady_clock::now() + kDuration;

        while (std::chrono::steady_clock::now() < deadline &&
               !stop.load(std::memory_order_acquire)) {
            ring.resize(capacityDist(rng), noPrepare);
            swaps.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        stop.store(true, std::memory_order_release);
    });

    writer.join();
    reader.join();
    resizer.join();

    EXPECT_FALSE(corrupted.load(std::memory_order_acquire));
    EXPECT_GT(writes.load(std::memory_order_relaxed), 0u);
    EXPECT_GT(reads.load(std::memory_order_relaxed), 0u);
    EXPECT_GT(swaps.load(std::memory_order_relaxed), 100u);
}

TEST(ResizableRingBufferTest, FuzzResizeWithPrefill) {
    ResizableRingBuffer ring;
    ring.reset(128, noPrepare);

    std::mt19937 rng(0x1234abcd);
    std::uniform_int_distribution<size_t> capacityDist(32, 8192);
    std::vector<float> prefill(31);
    for (size_t i = 0; i < prefill.size(); ++i) {
        prefill[i] = static_cast<float>(i + 1);
    }

    for (int i = 0; i < 1000; ++i) {
        ring.resize(capacityDist(rng), noPrepare, prefill.data(), prefill.size());

        std::vector<float> readback(prefill.size());
        ASSERT_TRUE(ring.read(readback.data(), readback.size()));
        EXPECT_EQ(readback, prefill);
    }
}

} // namespace
} // namespace watermelon_audio::usb
