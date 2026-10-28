#pragma once

/**
 * @file WavFileDumpSink.h
 * @brief El borde de DISCO del volcado del afinador (REQ-055 S0 · 0.1): `open`/`fwrite`/renombre.
 *
 * El nucleo (`TunerInputDump`) decide si la toma es valida; esto solo la deja en un archivo con el
 * nombre que el nucleo dijo. Se escribe a `<path>.part` y se renombra al cerrar, asi que una toma
 * que murio a mitad (la app cae, se llena el disco) queda como `.part` y nunca con nombre de WAV.
 * Las muestras son los floats crudos de la CPU: el formato es little-endian y las plataformas del
 * motor (ARM64, x86-64) lo son.
 *
 * EL DIAGNOSTICO NO DESTRUYE ARCHIVOS (hallazgo 1 del security-auditor de REQ-055). El path lo arma
 * la app consumidora, y este sink nunca pisa lo que ya esta en el disco:
 *   - `openPart` rechaza si el nombre final (`<path>` o su `.invalid.wav`) ya existe, y crea el
 *     `.part` con `O_CREAT|O_EXCL|O_NOFOLLOW`: si ya existe, o es un symlink (aun colgante), falla
 *     sin truncar ni seguir nada.
 *   - `finishPart` renombra con `renameWithoutReplacing`: si el destino aparecio durante la toma,
 *     el `.part` queda, el path devuelto es "" (=> `finalized = 0`) y el destino no se toca.
 *   - `abortPart` (un start que devuelve false, bug B) borra el `.part` solo si sigue siendo el que
 *     `openPart` creo (dispositivo + inodo del descriptor `O_EXCL`); uno ajeno en ese nombre no se
 *     toca. Entre ese chequeo y el `unlink` queda una ventana (POSIX no tiene un unlink-si-es-este):
 *     alguien con permiso de escritura en el directorio que reemplace el `.part` justo ahi pierde
 *     su archivo. Es la misma clase de ventana que el ultimo escalon del renombre, y del mismo dueno.
 *
 * POSIX, no `std::`: es el borde de E/S (el nucleo no lo ve). Compila para Android, iOS/macOS y
 * el host Linux del CI; nada de `android/` ni `jni.h` (regla de portabilidad del repo).
 */

#include "TunerInputDump.h"

#include <cstdio>
#include <string>

namespace wma::analysis {

/// Los escalones del renombre sin reemplazo, en el orden en que se prueban (ver el .cpp). Se
/// exponen para que los tests ejerzan cada uno: en el host solo se alcanza el primero.
enum class NoReplaceRenameStep {
    kNative,           ///< renamex_np(RENAME_EXCL) en Apple; renameat2(RENAME_NOREPLACE) en Linux
    kLinkUnlink,       ///< link(from, to) + unlink(from): link nunca reemplaza
    kCheckThenRename,  ///< lstat(to) y rename: el TOCTOU entre los dos se acepta (ver el .cpp)
};

/// Como termino un escalon del renombre.
enum class RenameStepOutcome {
    kDone,          ///< renombrado
    kExists,        ///< el destino existe: no se toca nada, y no se baja de escalon
    kUnsupported,   ///< el kernel o el sistema de archivos no lo soporta: se prueba el siguiente
    kFailed,        ///< otro error (permiso, disco): no se insiste
};

/// La clasificacion de un errno de rename/link. Expuesta para que los tests fijen que un EINVAL
/// (FUSE, sdcardfs) baja de escalon y no aborta el nombrado.
RenameStepOutcome classifyRenameErrno(int err);

/// Un escalon reemplazable: los tests inyectan el nativo y el de link para alcanzar el paso a los
/// siguientes, que en el host nunca ocurre solo (ahi los dos se soportan). nullptr = el del sistema.
using RenameStepFn = RenameStepOutcome (*)(const std::string& from, const std::string& to);

/// Renombra `from` a `to` SIN reemplazar nunca un `to` existente (archivo, directorio o symlink).
/// Empieza por `firstStep` y baja al siguiente escalon solo si el anterior no esta soportado por el
/// kernel o el sistema de archivos. false si `to` existe o si algo fallo: `from` queda donde estaba.
bool renameWithoutReplacing(const std::string& from, const std::string& to,
                            NoReplaceRenameStep firstStep = NoReplaceRenameStep::kNative,
                            RenameStepFn native = nullptr, RenameStepFn link = nullptr);

class WavFileDumpSink final : public IDumpSink {
public:
    WavFileDumpSink() = default;
    ~WavFileDumpSink() override;

    WavFileDumpSink(const WavFileDumpSink&) = delete;
    WavFileDumpSink& operator=(const WavFileDumpSink&) = delete;

    /// Crea `<path>.part` en exclusiva y escribe `headerBytes` ceros (el header real se parchea al
    /// cerrar). false, sin tocar nada, si `<path>` o `invalidDumpPathFor(path)` ya existen, o si
    /// `<path>.part` ya existe o es un symlink.
    bool openPart(const std::string& path, size_t headerBytes) override;
    bool appendSamples(const float* samples, size_t count) override;
    /// Parchea el header en el offset 0, cierra y renombra SIN REEMPLAZAR a `path` (valid) o a
    /// `invalidDumpPathFor(path)`. "" si algo fallo, incluido un destino que ya existe: entonces el
    /// `.part` queda con los datos y el destino intacto.
    std::string finishPart(const uint8_t* header, size_t headerBytes, bool valid) override;
    /// `<path>.part` desde un `openPart` exitoso; "" si nunca abrio (o se aborto).
    std::string partPath() const override;
    /// Cierra y borra `<path>.part` SOLO si sigue siendo el archivo que `openPart` creo (mismo
    /// dispositivo e inodo que el descriptor creado con `O_EXCL`). Si en ese nombre hay otro
    /// archivo, no lo toca y devuelve false. Ver el contrato en `IDumpSink::abortPart`.
    bool abortPart() override;

private:
    void closeFile() noexcept;

    std::FILE* mFile = nullptr;
    std::string mPath;
    /// La identidad del `.part` que `openPart` creo: lo unico que `abortPart` borra.
    unsigned long long mPartDev = 0;
    unsigned long long mPartIno = 0;
};

}  // namespace wma::analysis
