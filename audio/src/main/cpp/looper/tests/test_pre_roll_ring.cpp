// Validates the pre-roll ring buffer: write/snapshot semantics, leading
// zero-padding when history is shorter than the request, and wrap-around.
#include <gtest/gtest.h>
#include "PreRollRing.h"

#include <vector>

namespace {

// Fill a stereo buffer with an ascending ramp keyed off `startFrame`. Both
// channels carry the same value so we can spot misalignment as duplication.
std::vector<float> makeRamp(int startFrame, int numFrames) {
    std::vector<float> buf(static_cast<size_t>(numFrames) * 2);
    for (int i = 0; i < numFrames; ++i) {
        const float v = static_cast<float>(startFrame + i);
        buf[i * 2]     = v;
        buf[i * 2 + 1] = v;
    }
    return buf;
}

}  // namespace

TEST(PreRollRing, CapacityReportedAfterPrepare) {
    PreRollRing ring;
    ring.prepare(4800);
    EXPECT_EQ(ring.getCapacityFrames(), 4800);
}

TEST(PreRollRing, SnapshotWithoutWritesReturnsZeros) {
    PreRollRing ring;
    ring.prepare(1024);
    std::vector<float> out(256 * 2, 99.0f);  // sentinel
    int got = ring.snapshot(out.data(), 256);
    EXPECT_EQ(got, 256);
    for (float s : out) EXPECT_FLOAT_EQ(s, 0.0f);
}

TEST(PreRollRing, SnapshotPadsLeadingZerosWhenHistoryShort) {
    PreRollRing ring;
    ring.prepare(1024);
    auto in = makeRamp(/*startFrame=*/0, /*numFrames=*/100);
    ring.write(in.data(), 100);

    // Request 256 frames; only 100 should be present, padded with 156 zeros.
    std::vector<float> out(256 * 2, 99.0f);
    EXPECT_EQ(ring.snapshot(out.data(), 256), 256);

    const int padFrames = 256 - 100;
    for (int i = 0; i < padFrames; ++i) {
        EXPECT_FLOAT_EQ(out[i * 2],     0.0f) << "i=" << i;
        EXPECT_FLOAT_EQ(out[i * 2 + 1], 0.0f);
    }
    for (int i = 0; i < 100; ++i) {
        const float expected = static_cast<float>(i);
        EXPECT_FLOAT_EQ(out[(padFrames + i) * 2],     expected);
        EXPECT_FLOAT_EQ(out[(padFrames + i) * 2 + 1], expected);
    }
}

TEST(PreRollRing, SnapshotReturnsMostRecentFrames) {
    PreRollRing ring;
    ring.prepare(1024);
    auto in = makeRamp(0, 1024);
    ring.write(in.data(), 1024);

    // Request 128 most-recent frames: should be values 896..1023.
    std::vector<float> out(128 * 2);
    EXPECT_EQ(ring.snapshot(out.data(), 128), 128);
    for (int i = 0; i < 128; ++i) {
        EXPECT_FLOAT_EQ(out[i * 2], static_cast<float>(896 + i));
    }
}

TEST(PreRollRing, WrapAroundPreservesOrder) {
    PreRollRing ring;
    ring.prepare(512);
    // Write 800 frames so the ring wraps. The last 512 frames live in the ring.
    auto in = makeRamp(0, 800);
    ring.write(in.data(), 800);

    // Snapshot the full capacity. Most-recent 512 frames are values 288..799.
    std::vector<float> out(512 * 2);
    EXPECT_EQ(ring.snapshot(out.data(), 512), 512);
    for (int i = 0; i < 512; ++i) {
        EXPECT_FLOAT_EQ(out[i * 2], static_cast<float>(288 + i)) << "i=" << i;
    }
}

TEST(PreRollRing, SnapshotClampedToCapacity) {
    PreRollRing ring;
    ring.prepare(256);
    auto in = makeRamp(0, 256);
    ring.write(in.data(), 256);

    // Request more than capacity — must clamp to capacity.
    std::vector<float> out(512 * 2);
    int got = ring.snapshot(out.data(), 512);
    EXPECT_EQ(got, 256);
}

TEST(PreRollRing, MultipleWritesAccumulate) {
    PreRollRing ring;
    ring.prepare(1024);
    // Four consecutive 64-frame writes — together 256 frames of ramp 0..255.
    for (int chunk = 0; chunk < 4; ++chunk) {
        auto in = makeRamp(chunk * 64, 64);
        ring.write(in.data(), 64);
    }
    std::vector<float> out(256 * 2);
    EXPECT_EQ(ring.snapshot(out.data(), 256), 256);
    for (int i = 0; i < 256; ++i) {
        EXPECT_FLOAT_EQ(out[i * 2], static_cast<float>(i));
    }
}
