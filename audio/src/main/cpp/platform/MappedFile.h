#pragma once

/**
 * @file MappedFile.h
 * @brief Un archivo (o una sub-region de un fd) mapeado en memoria, de solo lectura. RAII.
 *
 * El mapeo vive en `platform/` porque `mmap` y `CreateFileMappingW` no se parecen en nada:
 * `MappedFilePosix.cpp` es el codigo que `SoundFontManager` tenia inline, movido sin cambios;
 * `MappedFileWindows.cpp` es la otra rama. Sin headers de SO aca, a proposito.
 *
 * Los paths son UTF-8 (REQ-061 D5); en Windows se convierten a UTF-16 adentro.
 *
 * NO es RT-safe (abre archivos y loguea): control / JNI thread.
 */

#include <cstddef>
#include <cstdint>
#include <utility>

namespace wma { namespace platform {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { release(); }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept { swapWith(other); }
    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            release();
            swapWith(other);
        }
        return *this;
    }

    /**
     * Mapea el archivo ENTERO. Invalido (`isMapped() == false`, con un log) si no existe, no se puede
     * abrir, esta vacio o el path no es UTF-8 valido (ninguna de las dos ramas filtra el tipo de
     * archivo). El archivo se cierra antes de volver: el mapeo no retiene ningun handle.
     */
    static MappedFile mapPath(const char* utf8Path);

    /**
     * Mapea `[offset, offset+length)` de un descriptor de archivo POSIX, alineando el mapeo a la
     * pagina por dentro (`bytes()` apunta al `offset` pedido, no al de la pagina). El fd es del
     * llamador y puede cerrarse al volver.
     *
     * En Windows SIEMPRE es invalido, con un log que nombra la razon: la JVM no entrega fds de CRT.
     */
    static MappedFile mapFdRegion(int fd, int64_t offset, int64_t length);

    bool isMapped() const { return mBytes != nullptr; }
    /// El primer byte pedido; nullptr si no hay mapeo.
    const uint8_t* bytes() const { return mBytes; }
    /// Los bytes pedidos (el largo del archivo, o `length` de la region); 0 si no hay mapeo.
    size_t byteCount() const { return mByteCount; }

    /// Suelta el mapeo. Idempotente.
    void release();

private:
    void swapWith(MappedFile& other) noexcept {
        std::swap(mBase, other.mBase);
        std::swap(mMapLength, other.mMapLength);
        std::swap(mBytes, other.mBytes);
        std::swap(mByteCount, other.mByteCount);
    }

    void* mBase = nullptr;        ///< lo que se le devuelve al SO al soltar
    size_t mMapLength = 0;        ///< el largo que mapeo el SO (POSIX lo necesita para munmap)
    const uint8_t* mBytes = nullptr;
    size_t mByteCount = 0;
};

}} // namespace wma::platform
