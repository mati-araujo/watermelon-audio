#pragma once

/**
 * @file WavFileDumpSink.h
 * @brief El borde de DISCO del volcado del afinador (REQ-055 S0 · 0.1): `fopen`/`fwrite`/`rename`.
 *
 * El nucleo (`TunerInputDump`) decide si la toma es valida; esto solo la deja en un archivo con el
 * nombre que el nucleo dijo. Se escribe a `<path>.part` y se renombra al cerrar, asi que una toma
 * que murio a mitad (la app cae, se llena el disco) queda como `.part` y nunca con nombre de WAV.
 * Las muestras son los floats crudos de la CPU: el formato es little-endian y las plataformas del
 * motor (ARM64, x86-64) lo son.
 */

#include "TunerInputDump.h"

#include <cstdio>

namespace wma::analysis {

class WavFileDumpSink final : public IDumpSink {
public:
    WavFileDumpSink() = default;
    ~WavFileDumpSink() override;

    WavFileDumpSink(const WavFileDumpSink&) = delete;
    WavFileDumpSink& operator=(const WavFileDumpSink&) = delete;

    /// Escribe `headerBytes` ceros en `<path>.part`: el header real se parchea al cerrar.
    bool openPart(const std::string& path, size_t headerBytes) override;
    bool appendSamples(const float* samples, size_t count) override;
    /// Parchea el header en el offset 0, cierra y renombra a `path` (valid) o a
    /// `invalidDumpPathFor(path)`. "" si algo fallo, incluido el rename.
    std::string finishPart(const uint8_t* header, size_t headerBytes, bool valid) override;

private:
    void closeFile() noexcept;

    std::FILE* mFile = nullptr;
    std::string mPath;
};

}  // namespace wma::analysis
