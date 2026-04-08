#include <gtest/gtest.h>
#include "LockFreeRingBuffer.h"
#include <thread>
#include <atomic>

TEST(LockFreeRingBuffer, WriteAndRead) {
    LockFreeRingBuffer buffer(1024);

    float writeData[] = {1.0f, 2.0f, 3.0f, 4.0f};
    EXPECT_TRUE(buffer.write(writeData, 4));

    float readData[4] = {};
    EXPECT_TRUE(buffer.read(readData, 4));

    EXPECT_FLOAT_EQ(readData[0], 1.0f);
    EXPECT_FLOAT_EQ(readData[1], 2.0f);
    EXPECT_FLOAT_EQ(readData[2], 3.0f);
    EXPECT_FLOAT_EQ(readData[3], 4.0f);
}

TEST(LockFreeRingBuffer, EmptyReadFails) {
    LockFreeRingBuffer buffer(1024);
    float readData[4] = {};
    EXPECT_FALSE(buffer.read(readData, 4));
}

TEST(LockFreeRingBuffer, FullWriteFails) {
    LockFreeRingBuffer buffer(16);

    float data[15] = {};
    // capacity-1 should succeed (ring buffer needs 1 slot gap)
    EXPECT_TRUE(buffer.write(data, 15));
    // One more should fail
    float extra[1] = {99.0f};
    EXPECT_FALSE(buffer.write(extra, 1));
}

TEST(LockFreeRingBuffer, Wraparound) {
    LockFreeRingBuffer buffer(32);

    float writeData[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    float readData[8] = {};

    for (int cycle = 0; cycle < 20; cycle++) {
        EXPECT_TRUE(buffer.write(writeData, 8));
        EXPECT_TRUE(buffer.read(readData, 8));

        for (int i = 0; i < 8; i++) {
            EXPECT_FLOAT_EQ(readData[i], writeData[i])
                << "Mismatch at cycle " << cycle << " sample " << i;
        }
    }
}

TEST(LockFreeRingBuffer, AvailableToRead) {
    LockFreeRingBuffer buffer(1024);

    EXPECT_EQ(buffer.availableToRead(), 0u);

    float data[100] = {};
    buffer.write(data, 100);
    EXPECT_EQ(buffer.availableToRead(), 100u);

    buffer.read(data, 50);
    EXPECT_EQ(buffer.availableToRead(), 50u);
}

TEST(LockFreeRingBuffer, ConcurrentSPSC) {
    LockFreeRingBuffer buffer(4096);
    std::atomic<bool> done{false};
    std::atomic<int> totalRead{0};
    constexpr int TOTAL_SAMPLES = 64 * 1000;

    std::thread producer([&]() {
        float data[64];
        int written = 0;
        while (written < TOTAL_SAMPLES) {
            for (int i = 0; i < 64; i++) {
                data[i] = static_cast<float>(written + i);
            }
            if (buffer.write(data, 64)) {
                written += 64;
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        float data[64];
        int total = 0;
        while (!done.load(std::memory_order_acquire) || buffer.availableToRead() >= 64) {
            if (buffer.read(data, 64)) {
                total += 64;
                for (int i = 1; i < 64; i++) {
                    EXPECT_FLOAT_EQ(data[i], data[i-1] + 1.0f);
                }
            }
        }
        totalRead.store(total, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    EXPECT_GT(totalRead.load(), 0);
}
