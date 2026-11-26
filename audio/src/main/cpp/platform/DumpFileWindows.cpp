#include "DumpFile.h"
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

#include <fcntl.h>
#include <io.h>

// Windows implementation of the dump's disk primitives (REQ-061 AC-061.4). It gives the guarantees
// DumpFilePosix.cpp gets from O_EXCL|O_NOFOLLOW, link(2) and lstat():
//   - the `.part` is CREATE_NEW with FILE_FLAG_OPEN_REPARSE_POINT: an existing name (a symlink or a
//     junction, even dangling) fails, and a reparse point is opened as itself, never traversed. Without
//     that flag CREATE_NEW on a reparse point nobody handles fails with ERROR_CANT_ACCESS_FILE instead
//     of ERROR_FILE_EXISTS (measured: that is the only observable difference; a junction gives
//     ERROR_ACCESS_DENIED with or without it, being a directory);
//   - the rename is MoveFileExW without MOVEFILE_REPLACE_EXISTING and without MOVEFILE_COPY_ALLOWED:
//     it never overwrites and never degrades to copy + delete;
//   - only the `.part` we created is deleted, checked by volume + file index on the very handle that
//     deletes it.
// Every failure path closes: a path that is not valid UTF-8 touches nothing.

namespace wma { namespace platform {

namespace {

using Outcome = RenameStepOutcome;

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

bool isNotThere(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

unsigned long long fileIndexOf(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

Outcome moveWithoutReplacing(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (from.empty() || to.empty()) return Outcome::kFailed;
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return Outcome::kDone;
    const DWORD error = ::GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) return Outcome::kExists;
    return Outcome::kFailed;
}

}  // namespace

bool dumpPathMayExist(const std::string& path) {
    const std::filesystem::path wide = pathFromUtf8(path.c_str());
    if (wide.empty()) return true;   // not UTF-8: refuse to say it is free
    // Open the entry itself (reparse point and all), asking for nothing: attributes only.
    const HANDLE h = ::CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
        return true;
    }
    return !isNotThere(::GetLastError());
}

std::FILE* dumpCreatePartExclusive(const std::string& part, PartIdentity& identity) {
    const std::filesystem::path wide = pathFromUtf8(part.c_str());
    if (wide.empty()) return nullptr;
    // Share mode 0: nobody else opens the take while it is alive. No SECURITY_ATTRIBUTES: the handle is
    // not inheritable (the equivalent of O_CLOEXEC).
    const HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info)) {
        ::CloseHandle(h);
        ::DeleteFileW(wide.c_str());   // created just now, exclusively: it is ours
        return nullptr;
    }
    identity.device = info.dwVolumeSerialNumber;
    identity.inode = fileIndexOf(info);

    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        ::CloseHandle(h);
        ::DeleteFileW(wide.c_str());
        return nullptr;
    }
    std::FILE* file = ::_fdopen(fd, "wb");   // owns fd, and through it the handle
    if (file == nullptr) {
        ::_close(fd);
        ::DeleteFileW(wide.c_str());
        return nullptr;
    }
    return file;
}

void dumpDiscardCreatedPart(const std::string& part) {
    const std::filesystem::path wide = pathFromUtf8(part.c_str());
    if (!wide.empty()) ::DeleteFileW(wide.c_str());
}

bool dumpRemovePartIfSame(const std::string& part, const PartIdentity& identity) {
    const std::filesystem::path wide = pathFromUtf8(part.c_str());
    if (wide.empty()) return false;
    const HANDLE h = ::CreateFileW(wide.c_str(), DELETE | FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return isNotThere(::GetLastError());   // already gone: nothing is left

    BY_HANDLE_FILE_INFORMATION info{};
    const bool ours = ::GetFileInformationByHandle(h, &info) &&
                      info.dwVolumeSerialNumber == identity.device &&
                      fileIndexOf(info) == identity.inode &&
                      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0;
    bool removed = false;
    if (ours) {
        // Delete through the handle that was just checked: nothing can be swapped in between.
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        removed = ::SetFileInformationByHandle(h, FileDispositionInfo, &disposition,
                                               sizeof(disposition)) != 0;
    }
    ::CloseHandle(h);
    return removed;
}

Outcome dumpRenameNative(const std::string& from, const std::string& to) {
    return moveWithoutReplacing(pathFromUtf8(from.c_str()), pathFromUtf8(to.c_str()));
}

/// There is no link(2) step here: MoveFileExW without REPLACE_EXISTING is already the atomic
/// no-replace, so the chain goes straight on to the last step.
Outcome dumpRenameByLink(const std::string&, const std::string&) { return Outcome::kUnsupported; }

Outcome dumpRenameAfterCheck(const std::string& from, const std::string& to) {
    if (dumpPathMayExist(to)) return Outcome::kExists;
    return moveWithoutReplacing(pathFromUtf8(from.c_str()), pathFromUtf8(to.c_str()));
}

}} // namespace wma::platform
