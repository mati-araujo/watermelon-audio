#pragma once

#include <cstdint>

/**
 * @file SoundFontFdRegion.h
 * @brief Pure, dependency-free math for mmap'ing a sub-region of a file
 *        descriptor (fd + offset + length).
 *
 * Extracted from SoundFontManager::loadFromFd so the riskiest part — bounds
 * validation and page-alignment of the mmap offset — is unit-testable on the
 * host without POSIX (sys/mman.h) or TinySoundFont, both of which are
 * unavailable in the host googletest build.
 *
 * mmap() requires its `offset` argument to be a multiple of the page size.
 * Play Asset Delivery / AssetFileDescriptor offsets are NOT page-aligned, so
 * we align the offset DOWN to a page boundary, extend the mapping length by
 * the same delta, and hand TinySoundFont a pointer `delta` bytes into the
 * mapping.
 */
namespace wma {

/**
 * @brief Result of computing the page-aligned mmap region for [offset, offset+length).
 */
struct MmapRegion {
    int64_t alignedOffset = 0;  ///< Page-aligned offset to pass to mmap().
    int64_t mapLength = 0;      ///< Bytes to map (>= length; covers the delta).
    int64_t dataDelta = 0;      ///< Byte offset of the SF data within the mapping.
};

/**
 * @brief Validate [offset, offset+length) against a file size and compute the
 *        page-aligned mmap region.
 *
 * @param fileSize  Total size of the underlying file, in bytes (from fstat).
 * @param offset    Byte offset of the SoundFont region within the file.
 * @param length    Length of the SoundFont region, in bytes.
 * @param pageSize  System page size (must be a power of two > 0).
 * @param out       Populated only when the function returns true.
 * @return true if the region is valid and `out` was populated; false (leaving
 *         `out` untouched) for any invalid input or out-of-range region.
 *
 * Guarantees when true:
 *   - out.alignedOffset <= offset  and is a multiple of pageSize
 *   - out.dataDelta == offset - out.alignedOffset  (0 <= dataDelta < pageSize)
 *   - out.alignedOffset + out.mapLength == offset + length  (never past EOF)
 *
 * Overflow-safe: the bounds check is written as `length > fileSize - offset`
 * (after guaranteeing offset <= fileSize) so offset+length never overflows.
 */
inline bool computeSoundFontMmapRegion(int64_t fileSize, int64_t offset,
                                       int64_t length, int64_t pageSize,
                                       MmapRegion& out) {
    if (fileSize < 0 || offset < 0 || length <= 0 || pageSize <= 0) {
        return false;
    }
    // pageSize must be a power of two for the mask-based alignment below.
    if ((pageSize & (pageSize - 1)) != 0) {
        return false;
    }
    if (offset > fileSize) {
        return false;
    }
    if (length > fileSize - offset) {  // overflow-safe: offset <= fileSize here
        return false;
    }

    const int64_t alignedOffset = offset & ~(pageSize - 1);
    const int64_t delta = offset - alignedOffset;

    out.alignedOffset = alignedOffset;
    out.dataDelta = delta;
    out.mapLength = length + delta;
    return true;
}

}  // namespace wma
