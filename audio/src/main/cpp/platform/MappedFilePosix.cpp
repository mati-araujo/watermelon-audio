#include "MappedFile.h"

#undef WMA_LOG_TAG
#define WMA_LOG_TAG "SF8.Manager"
#include "Logger.h"
#include "../engines/SoundFontFdRegion.h"

#include <cerrno>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// POSIX (Android, iOS, macOS, Linux) implementation of MappedFile. This is the code SoundFontManager
// used to carry inline in loadFromPath() / loadFromFd(), MOVED unchanged (REQ-061 D6): the mmap
// calls, the madvise and the log lines are the ones that shipped.

// Darwin has no mmap64/off64_t: there off_t is unconditionally 64-bit, so plain
// mmap already gives what the *64 variants give on the 32-bit Android ABIs.
// See mapFdRegion() for why a 64-bit offset is required in the first place.
#if defined(__APPLE__)
#define WMA_MMAP ::mmap
using WmaMapOffset = ::off_t;
#else
#define WMA_MMAP ::mmap64
using WmaMapOffset = ::off64_t;
#endif

namespace wma { namespace platform {

MappedFile MappedFile::mapPath(const char* path) {
    MappedFile result;
    if (path == nullptr) {
        WMA_LOGE("[SF8] loadFromPath: null path");
        return result;
    }

    // Open file
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        WMA_LOGE("[SF8] loadFromPath: failed to open %s (errno=%d)", path, errno);
        return result;
    }

    // Get file size
    struct stat st{};
    if (fstat(fd, &st) < 0) {
        WMA_LOGE("[SF8] loadFromPath: fstat failed (errno=%d)", errno);
        close(fd);
        return result;
    }
    size_t fileSize = static_cast<size_t>(st.st_size);

    // mmap the file — read-only, private (copy-on-write if needed)
    void* mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); // fd can be closed after mmap

    if (mapped == MAP_FAILED) {
        WMA_LOGE("[SF8] loadFromPath: mmap failed for %zu bytes (errno=%d)", fileSize, errno);
        return result;
    }

    // Advise kernel: we'll read sequentially (improves readahead)
    madvise(mapped, fileSize, MADV_SEQUENTIAL);

    WMA_LOGI("[SF8] loadFromPath: mmap'd %zu bytes from %s", fileSize, path);

    result.mBase = mapped;
    result.mMapLength = fileSize;
    result.mBytes = static_cast<const uint8_t*>(mapped);
    result.mByteCount = fileSize;
    return result;
}

MappedFile MappedFile::mapFdRegion(int fd, int64_t offset, int64_t length) {
    MappedFile result;

    if (fd < 0) {
        WMA_LOGE("[SF8] loadFromFd: invalid fd=%d", fd);
        return result;
    }

    // Validate the region against the actual file size.
    struct stat st{};
    if (fstat(fd, &st) < 0) {
        WMA_LOGE("[SF8] loadFromFd: fstat failed (fd=%d, errno=%d)", fd, errno);
        return result;
    }

    wma::MmapRegion region{};
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (!wma::computeSoundFontMmapRegion(static_cast<int64_t>(st.st_size),
                                         offset, length,
                                         static_cast<int64_t>(pageSize),
                                         region)) {
        WMA_LOGE("[SF8] loadFromFd: region out of range "
                 "(offset=%lld, length=%lld, fileSize=%lld)",
                 static_cast<long long>(offset),
                 static_cast<long long>(length),
                 static_cast<long long>(st.st_size));
        return result;
    }

    // mmap the page-aligned region — read-only, private.
    // A 64-bit offset is mandatory here: off_t is 32-bit on the 32-bit
    // ABIs (armeabi-v7a, x86), which would truncate a large asset offset.
    // mapPath is immune (it always maps at offset 0); this path takes
    // an arbitrary offset. WMA_MMAP resolves to mmap64 where that matters
    // and to plain mmap on Darwin, where off_t is already 64-bit.
    void* mapped = WMA_MMAP(nullptr, static_cast<size_t>(region.mapLength),
                            PROT_READ, MAP_PRIVATE, fd,
                            static_cast<WmaMapOffset>(region.alignedOffset));
    if (mapped == MAP_FAILED) {
        WMA_LOGE("[SF8] loadFromFd: mmap failed for %lld bytes at offset %lld (errno=%d)",
                 static_cast<long long>(region.mapLength),
                 static_cast<long long>(region.alignedOffset), errno);
        return result;
    }

    // Advise kernel: we'll read sequentially (improves readahead)
    madvise(mapped, static_cast<size_t>(region.mapLength), MADV_SEQUENTIAL);

    WMA_LOGI("[SF8] loadFromFd: mmap'd %lld bytes (fd=%d, offset=%lld)",
             static_cast<long long>(length), fd,
             static_cast<long long>(offset));

    result.mBase = mapped;
    result.mMapLength = static_cast<size_t>(region.mapLength);
    // SF data starts `dataDelta` bytes into the (page-aligned) mapping.
    result.mBytes = static_cast<const uint8_t*>(mapped) + region.dataDelta;
    result.mByteCount = static_cast<size_t>(length);
    return result;
}

void MappedFile::release() {
    if (mBase != nullptr) munmap(mBase, mMapLength);
    mBase = nullptr;
    mMapLength = 0;
    mBytes = nullptr;
    mByteCount = 0;
}

}} // namespace wma::platform
