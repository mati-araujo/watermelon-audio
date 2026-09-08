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
#include "../../AnalysisRing.h"
#include "../../AnalysisThread.h"
#include "../../OfflineAnalysis.h"
#include "../../../looper/WavFile.h"
#include "SyntheticSignal.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wma_test::corpus {

/// Una linea del manifiesto: `nombre  sha256  hz_verdadero  descripcion`.
struct Entry {
    std::string name;
    double trueHz = 0.0;
    /**
     * Las cuerdas del instrumento del archivo, EN ORDEN DE CUERDA (REQ-032 S2). Salen del
     * prefijo del nombre (`guitarra-*`, `bajo-*`, `ukelele*`) y del catalogo COMPARTIDO, no de
     * una tabla propia. Vacio si el nombre no dice de que instrumento es: entonces se barre
     * sin instrumento declarado, como antes.
     *
     * Con candidatos es el camino donde el consumidor mide (R-API-49), y es el unico donde una
     * altura sin soporte cae en NO_LOCK y no en NO_SIGNAL (R-API-52).
     */
    std::vector<double> candidatesHz;
};

/// Lo que el analisis mostro sobre un archivo MIENTRAS LA NOTA SONABA.
struct Outcome {
    std::string name;
    double trueHz = 0.0;
    int sampleRate = 0;
    bool analysed = false;    ///< hubo al menos una publicacion
    bool published = false;   ///< y alguna trajo una lectura FINA de altura
    double cents = NAN;       ///< la ULTIMA lectura fina durante la nota, contra `trueHz`
    // REQ-032 S2 — lo que un consumidor VE, para que el reporte hable en sus terminos.
    int state = -1;               ///< estado de la ULTIMA publicacion de la nota
    double detectedHz = 0.0;      ///< altura gruesa de esa ultima publicacion (0 = ninguna)
    float spectralSupport = NAN;  ///< su indice 17: 1 / 0 / NaN
    double noteEndSec = 0.0;      ///< hasta donde se alimento el analisis (ver `noteEndFrames`)
    double lastReadingSec = 0.0;  ///< cuando fue la ultima lectura fina
    int publications = 0;         ///< cuantas publicaciones hubo
    int convergedWithoutSupport = 0;  ///< R-PITCH-37 sobre TODAS ellas: tiene que ser 0
};

/**
 * Hasta que frame SUENA la nota: el ultimo bloque de `kDrainFrames` frames con rms por encima de
 * `kNoteFloor`. Cero si ninguno.
 *
 * 🔴 POR QUE EL BARRIDO NO ALIMENTA EL ARCHIVO ENTERO (REQ-032 S1, medido). El puerto publica el
 * ULTIMO snapshot, y un render de FluidSynth trae la cola de release y despues silencio: la nota
 * de guitarra limpia decae por debajo de `kSilenceFloor` (0,001) antes de los 4 s —rms 0,0039 a
 * 2 s, 0,00089 a 4 s— y el archivo dura 7,3 s. Alimentarlo entero deja el ultimo snapshot en
 * NO_SIGNAL para los 41 archivos con altura: rojo en todos, y no era del corpus. El barrido tiene
 * que medir la NOTA, y "la nota" es lo que suena por encima del piso — lo mismo que un musico
 * escucha y lo mismo que el afinador mostro mientras sonaba.
 *
 * `kNoteFloor` es 2x el piso de silencio del motor: por debajo de 0,001 el motor ya declara
 * ausencia por nivel, asi que cortar ahi dejaria el ultimo bloque justo en la frontera. Medido
 * sobre los 44: la nota mas corta (ukelele_A4) suena 1,49 s por encima de 0,002; la mas larga
 * 5,11 s (hasta el note-off).
 */
constexpr float kNoteFloor = 0.002f;

inline int noteEndFrames(const wav::WavData& data) {
    const int block = wma::analysis::AnalysisThread::kDrainFrames;
    int end = 0;
    for (int start = 0; start + block <= data.numFrames; start += block) {
        double sumSq = 0.0;
        for (int i = start; i < start + block; ++i) {
            // el ring suma a mono con 0,5·(L+R): la misma mezcla que ve el analisis
            const double v = 0.5 * (static_cast<double>(data.buffer[static_cast<size_t>(i) * 2])
                                    + data.buffer[static_cast<size_t>(i) * 2 + 1]);
            sumSq += v * v;
        }
        if (std::sqrt(sumSq / block) >= kNoteFloor) end = start + block;
    }
    return end;
}

/// El instrumento que el NOMBRE declara, contra el catalogo compartido. Vacio si no dice.
inline std::vector<double> candidatesFor(const std::string& name) {
    const char* family = nullptr;
    if (name.rfind("guitarra", 0) == 0) family = "guitarra";
    else if (name.rfind("bajo", 0) == 0) family = "bajo";
    else if (name.rfind("ukelele", 0) == 0) family = "ukelele";
    std::vector<double> hz;
    if (family == nullptr) return hz;
    for (const auto& s : wma_test::catalogStrings())
        if (std::string(s.name).rfind(family, 0) == 0) hz.push_back(s.hz);
    return hz;
}

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
        out.push_back(Entry{std::string(name), hz, candidatesFor(name)});
    }
    std::fclose(f);
    return out;
}

/**
 * Analiza UN archivo con EL MISMO analisis que el puerto —`AnalysisThread::drainOnce()`, sin
 * thread y sin relojes— y se queda con lo que el afinador MOSTRO mientras la nota sonaba.
 *
 * 🔴 POR QUE NO `analyzeBuffer()` (REQ-032 S2, medido). El puerto devuelve el ULTIMO snapshot, y
 * en una nota real el ultimo es el peor: la cola decae hacia el piso, el strobe pierde señal y la
 * lectura fina desaparece justo antes de cortar. Con `analyzeBuffer` sobre la nota recortada,
 * `guitarra-acero_E4` (2,1 s de nota) terminaba en MEASURING sin lectura, habiendo mostrado una
 * convergida a 1,25 s. Un musico afina con lo que el afinador MUESTRA, no con el ultimo tick
 * antes del silencio; el consumidor mide igual (prefijos de 0,5 a 4 s). Asi que se drena tick a
 * tick, se lee cada publicacion, y se guarda la ULTIMA con lectura fina — y ademas se verifica
 * R-PITCH-37 sobre TODAS: ninguna puede ser CONVERGED con la bandera en 0.
 *
 * No reimplementa nada: es el cuerpo que corre el thread y el puerto (AC-015.3).
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

    // Se alimenta LA NOTA, no el archivo: ver `noteEndFrames`.
    const int frames = noteEndFrames(data);
    o.noteEndSec = static_cast<double>(frames) / data.sampleRate;
    if (frames <= 0) return o;

    wma::analysis::AnalysisRing ring;
    wma::analysis::AnalysisSnapshot snapshot;
    wma::analysis::AnalysisThread analysis(ring, snapshot);
    ring.setCaptureRate(data.sampleRate);
    analysis.setTargetHz(e.trueHz);
    // Con el instrumento declarado cuando el nombre lo dice (REQ-032 S2).
    analysis.setCandidates(e.candidatesHz.empty() ? nullptr : e.candidatesHz.data(),
                           static_cast<int>(e.candidatesHz.size()));

    const int capacity = static_cast<int>(wma::analysis::AnalysisRing::kCapacityFrames);
    int written = 0;
    while (written < frames) {
        const int chunk = (frames - written) < capacity ? (frames - written) : capacity;
        // `readWav` devuelve SIEMPRE estereo intercalado (duplica el mono): el layout del ring.
        ring.writeStereo(data.buffer.data() + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
        while (analysis.drainOnce() != wma::analysis::AnalysisThread::DrainOutcome::kRingEmpty) {
            float v[wma::analysis::kSnapshotValueCount];
            if (!snapshot.read(v)) continue;
            ++o.publications;
            o.analysed = true;
            o.state = static_cast<int>(v[wma::analysis::kSnapState]);
            o.detectedHz = static_cast<double>(v[wma::analysis::kSnapDetectedHz]);
            o.spectralSupport = v[wma::analysis::kSnapSpectralSupport];
            if (o.state == wma::analysis::kStateConverged && o.spectralSupport == 0.0f) {
                ++o.convergedWithoutSupport;
            }
            const float cents = v[wma::analysis::kSnapCents];
            if (!std::isnan(cents)) {
                o.published = true;
                o.cents = static_cast<double>(cents);
                o.lastReadingSec = static_cast<double>(written) / data.sampleRate;
            }
        }
    }
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
