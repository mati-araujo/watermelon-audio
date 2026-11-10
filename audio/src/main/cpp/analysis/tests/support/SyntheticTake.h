/**
 * SyntheticTake.h — REQ-055 S0 · 0.3 (D41). Tomas SINTETICAS del volcado del afinador, con cortes.
 *
 * La toma real del g42 (0.2) no tiene fecha; el alimentador (`TakeFeeder.h`) y la validacion del
 * manifiesto (`TakeManifest.h`) se prueban igual, sobre archivos con la forma EXACTA de un volcado:
 * el header lo escribe `writeInputDumpHeader` de PRODUCCION (`wmad` v2 con su mapa de cortes) y los
 * datos son float32 mono. Lo que se lee de vuelta lo lee un parser PROPIO (`parseWmadV2Chunk`), no
 * `InputDumpStats`: si el escritor y el lector compartieran codigo, un error en los dos no se veria.
 *
 * Los archivos van SIEMPRE a temporales (`/tmp/wma-corpus-*`, `corpus::makeTempDir`): nada de esto
 * se commitea, y en particular nunca `tests/corpus-g42-manifest.txt` (su aparicion hace FALLAR
 * `LatchInstrumentRealTake` a proposito).
 */
#pragma once

#include "../../TunerInputDump.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wma_test::take {

/// Un corte para armar una toma sintetica: perdida (`seam = false`: faltan `lost` frames justo antes
/// de `fileFrame`) o costura (`seam = true`: el salto cae en [fileFrame, fileFrame + span]).
struct SyntheticTakeCut {
    uint64_t fileFrame = 0;
    uint64_t lost = 0;
    uint32_t span = 0;
    bool seam = false;
};

/// Las stats de una toma VALIDA de `frames` frames a `rate` con esos cortes (en el orden dado), como
/// las cerraria el volcado: `framesLost` = suma de las perdidas, `discontinuities` = las costuras.
inline wma::analysis::InputDumpStats syntheticTakeStats(uint64_t frames, int rate,
                                                        const std::vector<SyntheticTakeCut>& cuts) {
    wma::analysis::InputDumpStats s;
    s.framesWritten = frames;
    s.sampleRate = rate;
    s.inputSource = 1;
    s.valid = true;
    for (const SyntheticTakeCut& c : cuts) {
        wma::analysis::InputDumpCut& d = s.cuts[s.cutCount++];
        d.fileFrame = c.fileFrame;
        d.lostFrames = c.seam ? 0 : c.lost;
        d.fileSpan = c.seam ? c.span : 0;
        d.kind = c.seam ? wma::analysis::kInputDumpCutSeam : wma::analysis::kInputDumpCutLoss;
        if (c.seam) ++s.discontinuities;
        else s.framesLost += c.lost;
    }
    return s;
}

/// Los bytes del archivo: el header de produccion (`writeInputDumpHeader`) + `mono` en float32 LE.
/// PRECONDICION: `s.framesWritten == mono.size()` (si no, el archivo miente y eso es lo que un test
/// de rechazo quiere: se arma igual).
inline std::vector<uint8_t> syntheticTakeBytes(const std::vector<float>& mono,
                                               const wma::analysis::InputDumpStats& s) {
    std::vector<uint8_t> out(wma::analysis::kInputDumpHeaderBytes + mono.size() * sizeof(float));
    wma::analysis::writeInputDumpHeader(out.data(), s);
    // float32 IEEE little-endian: el host de la suite (x86_64/arm64) ya es LE.
    if (!mono.empty()) std::memcpy(out.data() + wma::analysis::kInputDumpHeaderBytes, mono.data(),
                                   mono.size() * sizeof(float));
    return out;
}

/// Escribe `bytes` en `path`. false si no se pudo.
inline bool writeSyntheticTake(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

/// Un seno de `hz` y amplitud `amp` con un SALTO DE FASE real en cada frame de `jumps` (la fase
/// avanza `jumpRad` de golpe): lo que deja en el archivo una perdida o una costura de verdad.
inline std::vector<float> sineWithPhaseJumps(int frames, int rate, double hz, double amp,
                                             const std::vector<int64_t>& jumps, double jumpRad = 1.9) {
    std::vector<float> out(static_cast<size_t>(frames));
    double phase = 0.0;
    size_t j = 0;
    const double step = 2.0 * M_PI * hz / rate;
    for (int i = 0; i < frames; ++i) {
        while (j < jumps.size() && jumps[j] == i) {
            phase += jumpRad;
            ++j;
        }
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(phase));
        phase += step;
    }
    return out;
}

}  // namespace wma_test::take
