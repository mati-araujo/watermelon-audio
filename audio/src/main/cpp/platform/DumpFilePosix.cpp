#include "DumpFile.h"

#include <cstdlib>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <stdio.h>   // renamex_np, RENAME_EXCL
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>   // bionic, no `android/`: el nivel de API del device
#endif

// POSIX (Android, iOS, macOS, Linux) implementation of the dump's disk primitives. This is the code
// analysis/WavFileDumpSink.cpp carried inline, MOVED unchanged (REQ-061 D6): same calls, same
// order of the rename steps, same comments.

namespace wma { namespace platform {

using Outcome = RenameStepOutcome;

/// true salvo que `lstat` diga ENOENT: ante la duda (sin permiso para mirar), se asume ocupado.
bool dumpPathMayExist(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) return true;
    return errno != ENOENT;
}

std::FILE* dumpCreatePartExclusive(const std::string& part, PartIdentity& identity) {
    // O_EXCL: si ya existe (incluido un symlink, aun colgante) falla sin truncar ni seguir nada.
    // O_NOFOLLOW lo repite explicito. O_CLOEXEC: un exec del proceso no hereda el descriptor.
    const int fd = ::open(part.c_str(), O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) return nullptr;
    struct stat created{};
    if (::fstat(fd, &created) != 0) {
        ::close(fd);
        ::unlink(part.c_str());   // lo creamos recien, en exclusiva: es nuestro
        return nullptr;
    }
    identity.device = static_cast<unsigned long long>(created.st_dev);
    identity.inode = static_cast<unsigned long long>(created.st_ino);
    std::FILE* file = ::fdopen(fd, "wb");
    if (file == nullptr) {
        ::close(fd);
        ::unlink(part.c_str());   // lo creamos recien, en exclusiva: es nuestro
        return nullptr;
    }
    return file;
}

void dumpDiscardCreatedPart(const std::string& part) { ::unlink(part.c_str()); }

bool dumpRemovePartIfSame(const std::string& part, const PartIdentity& identity) {
    struct stat st{};
    if (::lstat(part.c_str(), &st) != 0) return errno == ENOENT;   // ya no esta: nada quedo
    // Solo el archivo que creamos con O_EXCL. Uno que lo reemplazo (u otro symlink) no es nuestro.
    if (static_cast<unsigned long long>(st.st_dev) != identity.device ||
        static_cast<unsigned long long>(st.st_ino) != identity.inode || !S_ISREG(st.st_mode)) {
        return false;
    }
    return ::unlink(part.c_str()) == 0 || errno == ENOENT;
}

namespace {

#if defined(__linux__) && defined(SYS_renameat2)
/// RENAME_NOREPLACE de <linux/fs.h>: no se incluye ese header (choca con <sys/mount.h> en algunas
/// libc) y el valor es ABI del kernel.
constexpr unsigned kRenameNoReplace = 1u << 0;

/// renameat2 solo donde la politica seccomp de la app lo deja pasar. Android filtra las syscalls de
/// las apps y una fuera de la lista MATA el proceso (SIGSYS), no devuelve ENOSYS. bionic expone
/// `renameat2` recien en la API 30, y con eso entra a su lista; en la 29 (el minSdk) no hay
/// garantia, asi que ahi se arranca por link(2). Fuera de Android (el host del CI) siempre.
bool renameat2Allowed() {
#if defined(__ANDROID__)
    static const bool allowed = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("ro.build.version.sdk", value) <= 0) return false;
        return std::atoi(value) >= 30;
    }();
    return allowed;
#else
    return true;
#endif
}
#endif

}  // namespace

Outcome dumpRenameNative(const std::string& from, const std::string& to) {
#if defined(__APPLE__)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return Outcome::kDone;
    return classifyRenameErrno(errno);
#elif defined(__linux__) && defined(SYS_renameat2)
    if (!renameat2Allowed()) return Outcome::kUnsupported;
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), kRenameNoReplace) == 0) {
        return Outcome::kDone;
    }
    return classifyRenameErrno(errno);
#else
    (void)from;
    (void)to;
    return Outcome::kUnsupported;
#endif
}

/// link(2) falla con EEXIST si el destino existe, sin carrera: es el no-reemplazo atomico de los
/// FS que no entienden los flags del nativo pero si enlaces duros.
Outcome dumpRenameByLink(const std::string& from, const std::string& to) {
    if (::link(from.c_str(), to.c_str()) != 0) return classifyRenameErrno(errno);
    if (::unlink(from.c_str()) == 0) return Outcome::kDone;
    // Quedaron los dos nombres sobre el mismo archivo: se deshace el nuevo, que es nuestro, para
    // que el estado sea "no se nombro" (el `.part` sigue con los datos) y no uno a medias.
    // Si tambien eso falla quedan los dos nombres sobre la toma (dos unlink fallidos seguidos en el
    // mismo directorio: no se vio nunca); se reporta como fallo igual, el `.part` tiene los datos.
    ::unlink(to.c_str());
    return Outcome::kFailed;
}

/// El ultimo recurso, para un FS sin flags de no-reemplazo NI enlaces duros (el FUSE del
/// almacenamiento externo de Android puede ser asi). Entre el `lstat` y el `rename` otro proceso
/// podria crear el destino y `rename` lo reemplazaria: ese TOCTOU SE ACEPTA. Para ganarlo hace
/// falta otro proceso con escritura en el mismo directorio, justo en esa ventana, y la app es quien
/// eligio el directorio; lo que este escalon cierra es el caso real (un nombre que ya estaba).
Outcome dumpRenameAfterCheck(const std::string& from, const std::string& to) {
    if (dumpPathMayExist(to)) return Outcome::kExists;
    if (::rename(from.c_str(), to.c_str()) == 0) return Outcome::kDone;
    return Outcome::kFailed;
}

}} // namespace wma::platform
