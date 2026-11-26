#pragma once

/**
 * @file DumpFile.h
 * @brief Las primitivas de DISCO del volcado del afinador (`analysis/WavFileDumpSink`) que dependen
 *        del sistema operativo (REQ-061 S2).
 *
 * `WavFileDumpSink` decide el protocolo (`<path>.part` -> renombre sin reemplazar); esto es lo que
 * toca el SO. `DumpFilePosix.cpp` es el codigo POSIX que el sink tenia inline, MOVIDO sin cambios
 * (`lstat`, `link`, `O_NOFOLLOW`, `O_CLOEXEC`, el orden link-antes-de-check); `DumpFileWindows.cpp`
 * da las mismas garantias con Win32 (hallazgo 1 del security-auditor de REQ-055):
 *   - el `.part` se crea en exclusiva y NUNCA se sigue un enlace (symlink o junction);
 *   - un destino que existe no se pisa nunca;
 *   - se borra solo el `.part` que se creo.
 *
 * Los paths son UTF-8 (REQ-061 D5). Sin headers de SO aca. NO es RT-safe.
 */

#include <cerrno>
#include <cstdio>
#include <string>

namespace wma { namespace platform {

/// Como termino un escalon del renombre sin reemplazo.
enum class RenameStepOutcome {
    kDone,          ///< renombrado
    kExists,        ///< el destino existe: no se toca nada, y no se baja de escalon
    kUnsupported,   ///< el kernel o el sistema de archivos no lo soporta: se prueba el siguiente
    kFailed,        ///< otro error (permiso, disco): no se insiste
};

/// La clasificacion de un errno de rename/link. Expuesta para que los tests fijen que un EINVAL
/// (FUSE, sdcardfs) baja de escalon y no aborta el nombrado.
inline RenameStepOutcome classifyRenameErrno(int err) {
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

/// La identidad del archivo que `dumpCreatePartExclusive` creo (dispositivo/volumen + inodo/indice).
struct PartIdentity {
    unsigned long long device = 0;
    unsigned long long inode = 0;
};

/// true salvo que el SO diga que NO existe: ante la duda (sin permiso para mirar) se asume ocupado.
/// No sigue enlaces: un symlink o una junction colgante cuentan como existentes.
bool dumpPathMayExist(const std::string& utf8Path);

/// Crea `utf8Part` en exclusiva y lo abre para escribir en binario: falla (nullptr, sin truncar ni
/// seguir nada) si ya existe, incluido un enlace aun colgante. En Windows, ademas, nadie mas lo abre
/// mientras viva. Llena `identity` con el archivo creado. El descriptor no se hereda en un exec.
std::FILE* dumpCreatePartExclusive(const std::string& utf8Part, PartIdentity& identity);

/// Borra `utf8Part`, que `dumpCreatePartExclusive` creo recien (un paso posterior fallo).
void dumpDiscardCreatedPart(const std::string& utf8Part);

/// Borra `utf8Part` SOLO si sigue siendo el archivo regular de `identity`. true si no queda nada
/// nuestro en ese nombre (se borro, o ya no estaba); false si hay otro archivo: no se toca.
bool dumpRemovePartIfSame(const std::string& utf8Part, const PartIdentity& identity);

/// Los escalones del renombre sin reemplazo (ver WavFileDumpSink.cpp). Ninguno reemplaza un `to`
/// que existe (archivo, directorio o enlace).
RenameStepOutcome dumpRenameNative(const std::string& from, const std::string& to);
RenameStepOutcome dumpRenameByLink(const std::string& from, const std::string& to);
RenameStepOutcome dumpRenameAfterCheck(const std::string& from, const std::string& to);

}} // namespace wma::platform
