/**
 * CorpusSweep.h — REQ-015 S3: el barrido del corpus grabado, SOBRE EL PUERTO.
 *
 * POR QUE ESTO EXISTE Y POR QUE NO PODIA EXISTIR ANTES
 * ----------------------------------------------------
 * REQ-001 10.7 pedia un barrido contra material grabado y quedo sin escribir por
 * una razon concreta: por el camino vivo, cada archivo cuesta arrancar un thread,
 * alimentar el ring en tiempo casi real y esperar por condicion. Con las 44
 * señales que hay disponibles eso son decenas de minutos, y un test asi no lo
 * corre nadie. Por el puerto cada archivo cuesta lo que cuesta analizarlo.
 *
 * 🔴 EL BARRIDO SE EJERCE SIEMPRE, AUNQUE EL CORPUS NO ESTE
 * ---------------------------------------------------------
 * El material grabado todavia no existe, y escribir un barrido que sólo corre el
 * dia que aparezca seria dejar el mecanismo sin llamador — exactamente el error
 * que REQ-014 S3 cometio con su contador. Por eso esta funcion se prueba contra
 * un corpus **sintetico** que el propio test genera (WAVs de verdad, checksums
 * de verdad, manifiesto de verdad), y el dia que llegue el corpus de campo corre
 * sobre el sin cambiar una linea.
 *
 * La DECISION de si hay corpus no vive aca: vive en `Corpus.h`, y sigue siendo
 * la misma — sin material se SALTEA, nunca se aprueba.
 */
#pragma once

#include "Corpus.h"

#include "../../AnalysisSnapshot.h"
#include "../../OfflineAnalysis.h"
#include "../../../looper/WavFile.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wma_test::corpus {

/// Una linea del manifiesto: `nombre  sha256  hz_verdadero  descripcion`.
struct Entry {
    std::string name;
    double trueHz = 0.0;
};

/// Lo que el puerto midio sobre un archivo.
struct Outcome {
    std::string name;
    double trueHz = 0.0;
    int sampleRate = 0;
    bool analysed = false;    ///< el puerto devolvio un snapshot
    bool published = false;   ///< y ese snapshot trae una lectura de altura
    double cents = NAN;       ///< desviacion contra `trueHz`, en cents
};

inline std::vector<Entry> entriesOf(const std::string& manifest) {
    std::vector<Entry> out;
    std::FILE* f = std::fopen(manifest.c_str(), "rb");
    if (f == nullptr) return out;

    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char name[512] = {0}, sha[256] = {0};
        double hz = 0.0;
        // El hz verdadero es el TERCER campo. Una linea sin el no se puede
        // barrer —no hay contra que comparar— y se saltea en vez de inventarle
        // un objetivo.
        if (std::sscanf(line, "%511s %255s %lf", name, sha, &hz) != 3) continue;
        if (!(hz > 0.0)) continue;
        out.push_back(Entry{std::string(name), hz});
    }
    std::fclose(f);
    return out;
}

/**
 * Analiza UN archivo por el puerto, midiendo contra su f0 declarado.
 *
 * El rate sale del ARCHIVO, no de una constante: el material grabado puede venir
 * a 44,1 o a 48, y analizarlo con el rate equivocado da una lectura bien formada
 * y equivocada por casi un semitono y medio.
 */
inline Outcome sweepFile(const std::string& path, const Entry& e) {
    Outcome o;
    o.name = e.name;
    o.trueHz = e.trueHz;

    const wav::WavData data = wav::readWav(path.c_str());
    if (data.numFrames <= 0 || data.sampleRate <= 0) return o;
    o.sampleRate = data.sampleRate;

    float values[wma::analysis::kSnapshotValueCount];
    // `readWav` devuelve SIEMPRE estereo intercalado (duplica el mono), que es
    // justo el layout que el puerto llama `channels = 2`.
    o.analysed = wma::analysis::analyzeBuffer(data.buffer.data(), data.numFrames,
                                              data.sampleRate, e.trueHz, values);
    if (!o.analysed) return o;

    o.cents = static_cast<double>(values[wma::analysis::kSnapCents]);
    o.published = !std::isnan(o.cents);
    return o;
}

inline std::vector<Outcome> sweepAll(const std::string& dir, const std::string& manifest) {
    std::vector<Outcome> out;
    for (const Entry& e : entriesOf(manifest)) {
        out.push_back(sweepFile(dir + "/" + e.name, e));
    }
    return out;
}

}  // namespace wma_test::corpus
