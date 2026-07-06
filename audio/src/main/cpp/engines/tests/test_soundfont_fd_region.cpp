// ============================================================================
// Unit tests for computeSoundFontMmapRegion (SoundFontFdRegion.h).
//
// Pure integer math — no POSIX mmap, no TinySoundFont — so it builds and runs
// in the host googletest suite where SoundFontManager itself cannot (it pulls
// sys/mman.h + tinysoundfont). This covers the riskiest part of loadFromFd:
// bounds validation and page-alignment of a non-aligned asset offset.
// ============================================================================

#include <gtest/gtest.h>

#include "SoundFontFdRegion.h"

using wma::computeSoundFontMmapRegion;
using wma::MmapRegion;

namespace {

constexpr int64_t kPage = 4096;

// ---- Valid regions ---------------------------------------------------------

TEST(SoundFontFdRegion, PageAlignedOffsetNeedsNoDelta) {
    MmapRegion r{};
    ASSERT_TRUE(computeSoundFontMmapRegion(/*fileSize=*/1'000'000,
                                           /*offset=*/8192,
                                           /*length=*/12'000, kPage, r));
    EXPECT_EQ(r.alignedOffset, 8192);
    EXPECT_EQ(r.dataDelta, 0);
    EXPECT_EQ(r.mapLength, 12'000);
}

TEST(SoundFontFdRegion, UnalignedOffsetAlignsDownAndExtendsLength) {
    // AssetFileDescriptor-style offset: not a multiple of the page size.
    MmapRegion r{};
    ASSERT_TRUE(computeSoundFontMmapRegion(/*fileSize=*/50'000'000,
                                           /*offset=*/10'000,
                                           /*length=*/12'000'000, kPage, r));
    // 10000 aligns down to 8192; delta = 1808.
    EXPECT_EQ(r.alignedOffset, 8192);
    EXPECT_EQ(r.dataDelta, 10'000 - 8192);
    EXPECT_EQ(r.mapLength, 12'000'000 + (10'000 - 8192));
    // Invariant: the mapping ends exactly at offset+length (never past EOF).
    EXPECT_EQ(r.alignedOffset + r.mapLength, 10'000 + 12'000'000);
}

TEST(SoundFontFdRegion, RegionEndingExactlyAtEofIsValid) {
    MmapRegion r{};
    ASSERT_TRUE(computeSoundFontMmapRegion(/*fileSize=*/20'000,
                                           /*offset=*/5000,
                                           /*length=*/15'000, kPage, r));
    EXPECT_EQ(r.alignedOffset, 4096);
    EXPECT_EQ(r.dataDelta, 904);
    EXPECT_EQ(r.alignedOffset + r.mapLength, 20'000);
}

TEST(SoundFontFdRegion, ZeroOffsetMapsFromStart) {
    MmapRegion r{};
    ASSERT_TRUE(computeSoundFontMmapRegion(1'000, 0, 1'000, kPage, r));
    EXPECT_EQ(r.alignedOffset, 0);
    EXPECT_EQ(r.dataDelta, 0);
    EXPECT_EQ(r.mapLength, 1'000);
}

// ---- Invalid regions (must return false, leaving out untouched) ------------

TEST(SoundFontFdRegion, RejectsZeroLength) {
    MmapRegion r{};
    EXPECT_FALSE(computeSoundFontMmapRegion(1'000, 0, 0, kPage, r));
}

TEST(SoundFontFdRegion, RejectsNegativeLength) {
    MmapRegion r{};
    EXPECT_FALSE(computeSoundFontMmapRegion(1'000, 0, -1, kPage, r));
}

TEST(SoundFontFdRegion, RejectsNegativeOffset) {
    MmapRegion r{};
    EXPECT_FALSE(computeSoundFontMmapRegion(1'000, -1, 10, kPage, r));
}

TEST(SoundFontFdRegion, RejectsOffsetPastEof) {
    MmapRegion r{};
    EXPECT_FALSE(computeSoundFontMmapRegion(1'000, 2'000, 10, kPage, r));
}

TEST(SoundFontFdRegion, RejectsRegionExtendingPastEof) {
    MmapRegion r{};
    // offset within file, but offset+length overruns.
    EXPECT_FALSE(computeSoundFontMmapRegion(1'000, 900, 200, kPage, r));
}

TEST(SoundFontFdRegion, RejectsRegionOneByteTooLong) {
    MmapRegion r{};
    EXPECT_FALSE(computeSoundFontMmapRegion(1'000, 0, 1'001, kPage, r));
}

TEST(SoundFontFdRegion, RejectsNonPowerOfTwoPageSize) {
    MmapRegion r{};
    EXPECT_FALSE(computeSoundFontMmapRegion(10'000, 100, 100, /*pageSize=*/4095, r));
}

TEST(SoundFontFdRegion, RejectsZeroPageSize) {
    MmapRegion r{};
    EXPECT_FALSE(computeSoundFontMmapRegion(10'000, 100, 100, /*pageSize=*/0, r));
}

TEST(SoundFontFdRegion, DoesNotOverflowNearInt64Max) {
    // offset huge, length huge — the overflow-safe check must reject without
    // computing offset+length (which would wrap).
    MmapRegion r{};
    constexpr int64_t kMax = INT64_MAX;
    EXPECT_FALSE(computeSoundFontMmapRegion(/*fileSize=*/kMax,
                                            /*offset=*/kMax - 10,
                                            /*length=*/1'000, kPage, r));
}

}  // namespace
