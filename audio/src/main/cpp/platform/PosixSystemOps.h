#pragma once

/**
 * @file PosixSystemOps.h
 * @brief Las llamadas de Linux/Android que `utils/ThreadUtils.h` y `utils/MemoryUtils.h` hacian
 *        directo (`setpriority`, `sysconf`, `mlock`, `madvise`), movidas aca sin cambios (REQ-061 S2).
 *
 * Existe para que `utils/` no nombre APIs de un solo SO (`scripts/check-cpp-portability.sh`, regla 2).
 * Header-only a proposito: `usb/` y los tests de USB incluyen `utils/` sin linkear ningun
 * `Platform*.cpp`, y esto no tiene nada que decidir por plataforma mas que "existe o no".
 *
 * Solo hay cuerpo en Linux/Android, que es donde `utils/` ya llamaba a esto; en el resto (Apple,
 * Windows) `utils/` toma su rama `#else`, que no llama a nada de aca. En Windows eso es una decision:
 * `VirtualLock` esta acotado por el working set minimo del proceso (falla con rangos grandes) y el
 * unico cliente de `MemoryUtils` es el camino USB, que es de Android.
 */

#if defined(__ANDROID__) || defined(__linux__)

#include <cerrno>
#include <cstddef>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

namespace wma { namespace platform {

/// `sysconf(_SC_NPROCESSORS_ONLN)`.
inline int sysOnlineCpuCount() { return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)); }

/// `setpriority(PRIO_PROCESS, tid, nice)`: el nice del thread `tid` (en Linux, `PRIO_PROCESS` es por thread).
inline bool sysSetThreadNice(pid_t tid, int nice) {
    return setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) == 0;
}

/// `mlock`. Si falla deja el `errno` de `mlock` en `err`.
inline bool sysLockPages(void* address, size_t size, int& err) {
    if (mlock(address, size) == 0) return true;
    err = errno;
    return false;
}

inline bool sysUnlockPages(void* address, size_t size) { return munlock(address, size) == 0; }

/// `sysconf(_SC_PAGESIZE)`.
inline size_t sysPageSize() { return static_cast<size_t>(sysconf(_SC_PAGESIZE)); }

inline bool sysAdviseWillNeed(void* alignedAddress, size_t size) {
    return madvise(alignedAddress, size, MADV_WILLNEED) == 0;
}

inline bool sysAdviseSequential(void* alignedAddress, size_t size) {
    return madvise(alignedAddress, size, MADV_SEQUENTIAL) == 0;
}

}} // namespace wma::platform

#endif
