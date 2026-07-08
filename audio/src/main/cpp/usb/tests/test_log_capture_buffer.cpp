// App V §3.2 — Host tests for the in-memory log capture ring.

#include <gtest/gtest.h>

#include "../../platform/LogCaptureBuffer.h"

using wma::LogCaptureBuffer;
using wma::LogLevel;

namespace {

TEST(LogCaptureBuffer, CapturesWhenEnabledOnly) {
    auto& b = LogCaptureBuffer::instance();
    b.clear();
    b.setEnabled(false);
    b.capture(LogLevel::INFO, "TAG", "dropped-while-disabled");
    EXPECT_TRUE(b.drain().empty());

    b.setEnabled(true);
    b.capture(LogLevel::INFO, "WMA_CLOCK", "hello");
    b.capture(LogLevel::WARN, "Libusb", "world");
    auto lines = b.drain();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "I/WMA_CLOCK: hello");
    EXPECT_EQ(lines[1], "W/Libusb: world");
    // Drain is destructive.
    EXPECT_TRUE(b.drain().empty());
    b.setEnabled(false);
    b.clear();
}

TEST(LogCaptureBuffer, RingDropsOldestAndCountsDropped) {
    auto& b = LogCaptureBuffer::instance();
    b.clear();
    b.setEnabled(true);
    const size_t over = LogCaptureBuffer::kCapacity + 100;
    for (size_t i = 0; i < over; ++i) {
        b.capture(LogLevel::DEBUG, "T", std::to_string(i).c_str());
    }
    EXPECT_EQ(b.droppedCount(), 100);
    auto lines = b.drain();
    EXPECT_EQ(lines.size(), LogCaptureBuffer::kCapacity);
    // Oldest 100 were dropped → first surviving line is index 100.
    EXPECT_EQ(lines.front(), "D/T: 100");
    EXPECT_EQ(lines.back(), "D/T: " + std::to_string(over - 1));
    b.setEnabled(false);
    b.clear();
}

}  // namespace
