#include "MappedFile.h"

#undef WMA_LOG_TAG
#define WMA_LOG_TAG "SF8.Manager"
#include "Logger.h"
#include "FileUtf8.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOGDI: wingdi.h defines ERROR, which is wma::LogLevel::ERROR in Logger.h.
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>

// Windows implementation of MappedFile (REQ-061 D5). Every call is the wide one: the path is UTF-8
// and FileUtf8.h turns it into UTF-16 (strictly: invalid UTF-8 maps nothing).

namespace wma { namespace platform {

MappedFile MappedFile::mapPath(const char* utf8Path) {
    MappedFile result;
    if (utf8Path == nullptr) {
        WMA_LOGE("[SF8] loadFromPath: null path");
        return result;
    }
    const std::filesystem::path wide = pathFromUtf8(utf8Path);
    if (wide.empty()) {
        WMA_LOGE("[SF8] loadFromPath: path is empty or not valid UTF-8: %s", utf8Path);
        return result;
    }

    // FILE_SHARE_READ only (no WRITE, no DELETE): fails closed if another process has the file open for
    // writing, so the bytes cannot change under the view. Truncating a mapped file is already refused by
    // Windows itself (ERROR_USER_MAPPED_FILE); the share mode is not what prevents that.
    const HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        WMA_LOGE("[SF8] loadFromPath: failed to open %s (error=%lu)", utf8Path,
                 static_cast<unsigned long>(::GetLastError()));
        return result;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        WMA_LOGE("[SF8] loadFromPath: GetFileSizeEx failed (error=%lu)",
                 static_cast<unsigned long>(::GetLastError()));
        ::CloseHandle(file);
        return result;
    }
    if (size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > static_cast<unsigned long long>(SIZE_MAX)) {
        // A section of length 0 does not exist: same as mmap(0), an empty file is not mappable.
        WMA_LOGE("[SF8] loadFromPath: cannot map %lld bytes from %s",
                 static_cast<long long>(size.QuadPart), utf8Path);
        ::CloseHandle(file);
        return result;
    }
    const size_t fileSize = static_cast<size_t>(size.QuadPart);

    const HANDLE section = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const DWORD sectionError = ::GetLastError();
    ::CloseHandle(file);   // the section keeps the file alive; the view keeps the section
    if (section == nullptr) {
        WMA_LOGE("[SF8] loadFromPath: CreateFileMappingW failed for %zu bytes (error=%lu)", fileSize,
                 static_cast<unsigned long>(sectionError));
        return result;
    }

    void* view = ::MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    const DWORD viewError = ::GetLastError();
    ::CloseHandle(section);
    if (view == nullptr) {
        WMA_LOGE("[SF8] loadFromPath: MapViewOfFile failed for %zu bytes (error=%lu)", fileSize,
                 static_cast<unsigned long>(viewError));
        return result;
    }

    WMA_LOGI("[SF8] loadFromPath: mapped %zu bytes from %s", fileSize, utf8Path);

    result.mBase = view;
    result.mMapLength = fileSize;
    result.mBytes = static_cast<const uint8_t*>(view);
    result.mByteCount = fileSize;
    return result;
}

MappedFile MappedFile::mapFdRegion(int fd, int64_t offset, int64_t length) {
    (void)offset;
    (void)length;
    WMA_LOGE("[SF8] loadFromFd: not supported on Windows: the JVM does not hand out CRT file "
             "descriptors (fd=%d); load the SoundFont by path", fd);
    return MappedFile();
}

void MappedFile::release() {
    if (mBase != nullptr) ::UnmapViewOfFile(mBase);
    mBase = nullptr;
    mMapLength = 0;
    mBytes = nullptr;
    mByteCount = 0;
}

}} // namespace wma::platform
