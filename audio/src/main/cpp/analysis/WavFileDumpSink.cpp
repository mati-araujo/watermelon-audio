#include "WavFileDumpSink.h"

#include <bit>
#include <cerrno>
#include <cstdlib>
#include <vector>

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

namespace wma::analysis {

static_assert(std::endian::native == std::endian::little,
              "el WAV es little-endian y las muestras se escriben tal cual las tiene la CPU");

RenameStepOutcome classifyRenameErrno(int err) {
    switch (err) {
        case EEXIST:
        case ENOTEMPTY:   // un directorio no vacio en el destino (algunos FS lo dicen asi)
            return RenameStepOutcome::kExists;
        case EINVAL:      // flag que el FS no entiende (FUSE, sdcardfs)
        case ENOSYS:      // syscall que el kernel no tiene
        case ENOTSUP:
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        case EOPNOTSUPP:
#endif
        case EPERM:       // link(2) en un FS sin enlaces duros (FAT, sdcardfs)
        case EACCES:      // link(2) negado por la politica (SELinux) donde rename(2) se permite
        case EMLINK:
        case EXDEV:
            return RenameStepOutcome::kUnsupported;
        default:
            return RenameStepOutcome::kFailed;
    }
}

namespace {

using Outcome = RenameStepOutcome;

/// true salvo que `lstat` diga ENOENT: ante la duda (sin permiso para mirar), se asume ocupado.
bool mayExist(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) return true;
    return errno != ENOENT;
}

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

Outcome renameNative(const std::string& from, const std::string& to) {
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
Outcome renameByLink(const std::string& from, const std::string& to) {
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
Outcome renameAfterCheck(const std::string& from, const std::string& to) {
    if (mayExist(to)) return Outcome::kExists;
    if (::rename(from.c_str(), to.c_str()) == 0) return Outcome::kDone;
    return Outcome::kFailed;
}

}  // namespace

bool renameWithoutReplacing(const std::string& from, const std::string& to,
                            NoReplaceRenameStep firstStep, RenameStepFn native,
                            RenameStepFn link) {
    switch (firstStep) {
        case NoReplaceRenameStep::kNative: {
            const Outcome o = native != nullptr ? native(from, to) : renameNative(from, to);
            if (o != Outcome::kUnsupported) return o == Outcome::kDone;
            [[fallthrough]];
        }
        case NoReplaceRenameStep::kLinkUnlink: {
            const Outcome o = link != nullptr ? link(from, to) : renameByLink(from, to);
            if (o != Outcome::kUnsupported) return o == Outcome::kDone;
            [[fallthrough]];
        }
        case NoReplaceRenameStep::kCheckThenRename:
            return renameAfterCheck(from, to) == Outcome::kDone;
    }
    return false;
}

WavFileDumpSink::~WavFileDumpSink() { closeFile(); }

void WavFileDumpSink::closeFile() noexcept {
    if (mFile != nullptr) {
        std::fclose(mFile);
        mFile = nullptr;
    }
}

bool WavFileDumpSink::openPart(const std::string& path, size_t headerBytes) {
    closeFile();
    mPath.clear();
    // El veredicto se sabe al cerrar: los dos nombres posibles tienen que estar libres ahora. Si
    // aparecen despues, `finishPart` igual se niega a pisarlos.
    if (mayExist(path) || mayExist(invalidDumpPathFor(path))) return false;

    const std::string part = path + ".part";
    // O_EXCL: si ya existe (incluido un symlink, aun colgante) falla sin truncar ni seguir nada.
    // O_NOFOLLOW lo repite explicito. O_CLOEXEC: un exec del proceso no hereda el descriptor.
    const int fd = ::open(part.c_str(), O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    mFile = ::fdopen(fd, "wb");
    if (mFile == nullptr) {
        ::close(fd);
        ::unlink(part.c_str());   // lo creamos recien, en exclusiva: es nuestro
        return false;
    }

    const std::vector<uint8_t> reserve(headerBytes, 0);
    if (std::fwrite(reserve.data(), 1, reserve.size(), mFile) != reserve.size()) {
        closeFile();
        ::unlink(part.c_str());
        return false;
    }
    mPath = path;
    return true;
}

bool WavFileDumpSink::appendSamples(const float* samples, size_t count) {
    if (mFile == nullptr) return false;
    return std::fwrite(samples, sizeof(float), count, mFile) == count;
}

std::string WavFileDumpSink::finishPart(const uint8_t* header, size_t headerBytes, bool valid) {
    if (mFile == nullptr) return "";

    bool ok = std::fseek(mFile, 0, SEEK_SET) == 0 &&
              std::fwrite(header, 1, headerBytes, mFile) == headerBytes;
    // `fclose` vacia el buffer de stdio: un error aca es un archivo truncado.
    ok = (std::fclose(mFile) == 0) && ok;
    mFile = nullptr;
    if (!ok) return "";

    const std::string finalPath = valid ? mPath : invalidDumpPathFor(mPath);
    if (!renameWithoutReplacing(mPath + ".part", finalPath)) return "";
    return finalPath;
}

}  // namespace wma::analysis
