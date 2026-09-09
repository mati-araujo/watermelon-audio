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
#include "../../PhaseSlopeEstimator.h"
#include "../../StrobeTracker.h"
#include "../../../looper/WavFile.h"
#include "SyntheticSignal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
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
    /**
     * La ULTIMA lectura fina durante la nota, contra `trueHz`, **en Hz absolutos**:
     * `objetivo · 2^(kSnapCents/1200)` llevado a cents contra el hz verdadero del manifiesto.
     *
     * 🔴 HASTA REQ-035 ESTO ERA `kSnapCents` A SECAS, Y ESTABA MAL. `kSnapCents` es relativo al
     * OBJETIVO del strobe, y con el instrumento declarado el modo rapido reengancha ese objetivo a
     * la cuerda del catalogo (el nominal temperado), no al `trueHz` que `sweepFile` fijo. Comparar
     * eso contra el oraculo absoluto media la desafinacion del propio sample (+5,78 c en ukelele
     * C4) y la atribuia al strobe: fue el "+5,40 c" de REQ-032 y la hipotesis entera de REQ-035,
     * refutada por la tabla de S1. Los cents publicados siguen en `strobeC`; el objetivo real, en
     * `strobeTargetHz`.
     */
    double cents = NAN;
    /// El estado publicado EN ESA publicacion (no en la ultima de la nota, que es la que decae).
    int readingState = -1;
    // REQ-032 S2 — lo que un consumidor VE, para que el reporte hable en sus terminos.
    int state = -1;               ///< estado de la ULTIMA publicacion de la nota
    double detectedHz = 0.0;      ///< altura gruesa de esa ultima publicacion (0 = ninguna)
    float spectralSupport = NAN;  ///< su indice 17: 1 / 0 / NaN
    double noteEndSec = 0.0;      ///< hasta donde se alimento el analisis (ver `noteEndFrames`)
    double lastReadingSec = 0.0;  ///< cuando fue la ultima lectura fina
    int publications = 0;         ///< cuantas publicaciones hubo
    int convergedWithoutSupport = 0;  ///< R-PITCH-37 sobre TODAS ellas: tiene que ser 0

    // REQ-035 S1 — LAS SONDAS DEL STROBE en la misma publicacion de la que sale `cents`.
    //
    // Se leen por `AnalysisThread::strobe()` (const, existe desde S7) sin tocar produccion. Lo
    // que el strobe NO expone y por eso no esta aca: `goertzelBinToRmsRatio` por parcial, que es
    // el piso de admision de REQ-027. Que parciales entraron al ajuste se RECONSTRUYE en cambio
    // (`admittedMask`): se prueba cada subconjunto de tamano `partialsUsed` de los parciales con
    // medicion contra `StrobeTracker::fitStretchedSeries` —la funcion de produccion, publica— y
    // se declara el que reproduce `strobeC` exacto. Con hasta 4 parciales son a lo sumo 6
    // subconjuntos, y el test AFIRMA que uno y solo uno lo reproduce.
    static constexpr int kPartials = wma::analysis::StrobeTracker::kPartials;
    double partialCents[kPartials] = {NAN, NAN, NAN, NAN};   ///< `partialCents(i)`, contra (i+1)·trueHz
    double partialSigma[kPartials] = {NAN, NAN, NAN, NAN};   ///< `partialUncertaintyCents(i)`
    bool partialMeasured[kPartials] = {false, false, false, false};
    int partialsUsed = 0;             ///< `partialsUsed()`: cuantos entraron a la combinacion
    double strobeC = NAN;             ///< `cents()` en double (kSnapCents lo redondea a float)
    /**
     * 🔴 `targetHz()` del strobe en esa publicacion. NO es `trueHz`: con el instrumento declarado
     * el modo rapido reengancha el strobe a la CUERDA DEL CATALOGO (el nominal temperado), asi que
     * `cents`/`strobeC` son relativos a ese nominal y no al hz_verdadero del manifiesto. La lectura
     * fina en Hz ABSOLUTOS es `strobeTargetHz · 2^(strobeC/1200)`, y esa es la que se compara con
     * el oraculo (`fineHz`, `fineVsTrueCents`).
     */
    double strobeTargetHz = NAN;
    double fineHz = NAN;              ///< la lectura fina en Hz absolutos
    double fineVsTrueCents = NAN;     ///< y contra `trueHz`: el error REAL de la fina
    double strobeSigmaC = NAN;        ///< `uncertaintyCents()`
    double coarseSeenByStrobe = NAN;  ///< `coarseDeviationCents()`: el control que arbitro
    double snapshotB = NAN;           ///< kSnapInharmonicityB (ajuste LINEAL ponderado de S7, no el del strobe)
    int admittedMask = -1;            ///< bit i = el parcial i entro al ajuste; -1 = no se pudo reconstruir
    double fitB = NAN;                ///< el B del ajuste reconstruido (el strobe no lo publica)
    /// (segundo, cents) de CADA publicacion con lectura fina: el eje del tiempo del error.
    std::vector<std::pair<double, double>> trajectory;

    /**
     * REQ-036 S1 — CADA publicacion de la nota, con lo que el strobe tenia en ese instante, y la
     * HISTORIA de fases de cada parcial reconstruida desde la sonda `regressionPhaseAt`.
     *
     * Existe para simular la compuerta y la ventana adaptativa DESDE AFUERA sobre material real,
     * antes de tocar produccion: con esto un test puede decir cuantas de las lecturas convergidas
     * de hoy dejarian de serlo y con que error quedarian las que siguen.
     *
     * La historia es contigua por TRAMOS: se corta donde produccion corto la integracion (un
     * `setTarget` del modo rapido, un `reset`, un hueco, o una ventana en silencio, que reinicia
     * el hilo de fase). `histSegment[p]` es el indice donde arranca el tramo vigente y
     * `histEnd[p]` cuantas fases hay hasta esta publicacion; la ventana de produccion en ese
     * instante es `history[p][histEnd − count, histEnd)`.
     */
    struct Publication {
        double sec = 0.0;
        int state = -1;                ///< el estado publicado
        bool fine = false;             ///< trajo lectura fina (kSnapCents no NaN)
        double centsAbs = NAN;         ///< la fina en Hz absolutos contra trueHz
        double strobeC = NAN;          ///< cents contra el objetivo del strobe
        double sigma = NAN;
        double targetHz = NAN;
        int used = 0;
        int admitted = -1;
        bool measured[kPartials] = {false, false, false, false};
        double pCents[kPartials] = {NAN, NAN, NAN, NAN};
        double pSigma[kPartials] = {NAN, NAN, NAN, NAN};
        int count[kPartials] = {0, 0, 0, 0};
        int histEnd[kPartials] = {0, 0, 0, 0};
        int histSegment[kPartials] = {0, 0, 0, 0};
    };
    std::vector<Publication> publicationLog;
    std::vector<double> history[kPartials];
};

/**
 * REQ-035 S1 — el ajuste de la serie estirada, ESPEJADO para que devuelva B.
 *
 * `StrobeTracker::fitStretchedSeries` publica C y σ pero se guarda B. Esto repite la misma busqueda
 * (seccion aurea sobre [0, kMaxInharmonicityB], kStretchFitIterations iteraciones, C = media de los
 * residuos) usando las constantes y `stretchCents` PUBLICAS del strobe. Que el espejo es fiel lo
 * verifica `reconstructAdmitted`: la C que sale de aca tiene que coincidir con la de produccion.
 */
inline bool fitStretchedSeriesWithB(const double* cents, const int* orders, int k,
                                    double* outC, double* outB) {
    using wma::analysis::StrobeTracker;
    if (k < StrobeTracker::kMinPartialsForStretchFit) return false;
    const auto sseFor = [&](double B, double* Cout) {
        double sum = 0.0;
        for (int i = 0; i < k; ++i) sum += cents[i] - StrobeTracker::stretchCents(B, orders[i]);
        const double C = sum / k;
        double sse = 0.0;
        for (int i = 0; i < k; ++i) {
            const double r = cents[i] - (C + StrobeTracker::stretchCents(B, orders[i]));
            sse += r * r;
        }
        if (Cout) *Cout = C;
        return sse;
    };
    constexpr double kPhi = 0.6180339887498949;
    double lo = 0.0, hi = StrobeTracker::kMaxInharmonicityB;
    double b1 = hi - kPhi * (hi - lo), b2 = lo + kPhi * (hi - lo);
    double f1 = sseFor(b1, nullptr), f2 = sseFor(b2, nullptr);
    for (int it = 0; it < StrobeTracker::kStretchFitIterations; ++it) {
        if (f1 < f2) { hi = b2; b2 = b1; f2 = f1; b1 = hi - kPhi * (hi - lo); f1 = sseFor(b1, nullptr); }
        else         { lo = b1; b1 = b2; f1 = f2; b2 = lo + kPhi * (hi - lo); f2 = sseFor(b2, nullptr); }
    }
    *outB = 0.5 * (lo + hi);
    sseFor(*outB, outC);
    return std::isfinite(*outC);
}

/**
 * Que subconjunto de los parciales medidos reproduce la C publicada. Devuelve la mascara (bit i =
 * parcial i) o −1 si ninguno o mas de uno la reproduce. `outB` es el B de ese ajuste.
 *
 * Reproduce la ENTRADA de `StrobeTracker::process`: parciales con medicion, σ finita y positiva,
 * cents finitos. Para k ≥ 2 la combinacion es `fitStretchedSeries`; con k = 1 es el valor solo.
 */
inline int reconstructAdmitted(const wma::analysis::StrobeTracker& s, int used, double publishedC,
                               double* outB) {
    using wma::analysis::StrobeTracker;
    constexpr int kP = StrobeTracker::kPartials;
    int found = -1;
    *outB = NAN;
    for (int mask = 1; mask < (1 << kP); ++mask) {
        if (__builtin_popcount(static_cast<unsigned>(mask)) != used) continue;
        double cents[kP], sigmas[kP];
        int orders[kP];
        int k = 0;
        bool ok = true;
        for (int i = 0; i < kP; ++i) {
            if (!(mask & (1 << i))) continue;
            const double sg = s.partialUncertaintyCents(i);
            if (!s.partialHasMeasurement(i) || !(sg > 0.0) || !std::isfinite(sg)
                || !std::isfinite(s.partialCents(i))) { ok = false; break; }
            cents[k] = s.partialCents(i); sigmas[k] = sg; orders[k] = i + 1; ++k;
        }
        if (!ok) continue;
        double C = NAN, sigmaC = NAN, B = NAN;
        if (k >= 2) {
            if (!StrobeTracker::fitStretchedSeries(cents, sigmas, orders, k, &C, &sigmaC)) continue;
            double mirrorC = NAN;
            if (!fitStretchedSeriesWithB(cents, orders, k, &mirrorC, &B)) continue;
            // El espejo tiene que ser fiel, o el B que devuelve no es el del strobe.
            if (std::fabs(mirrorC - C) > 1e-9) continue;
        } else {
            C = cents[0];
            B = 0.0;
        }
        if (std::fabs(C - publishedC) <= 1e-9 * std::max(1.0, std::fabs(publishedC))) {
            if (found != -1) return -1;   // ambiguo: dos subconjuntos dan la misma C
            found = mask;
            *outB = B;
        }
    }
    return found;
}

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
    // REQ-036 S1 — la reconstruccion de la historia de fases, por parcial.
    int lastWindows[Outcome::kPartials] = {0, 0, 0, 0};
    int lastCount[Outcome::kPartials] = {0, 0, 0, 0};
    int segment[Outcome::kPartials] = {0, 0, 0, 0};
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
            // REQ-036 S1 — el registro de ESTA publicacion, con o sin lectura fina.
            {
                const wma::analysis::StrobeTracker& s = analysis.strobe();
                Outcome::Publication pub;
                pub.sec = static_cast<double>(written) / data.sampleRate;
                pub.state = o.state;
                for (int i = 0; i < Outcome::kPartials; ++i) {
                    const wma::analysis::PhaseSlopeEstimator& e = s.partialEstimator(i);
                    const int windows = e.windowsAnalyzed();
                    const int count = e.regressionPhaseCount();
                    std::vector<double>& h = o.history[i];
                    const int fresh = windows - lastWindows[i];
                    // Un `reset`/`setTarget` deja windows en cero; una ventana en silencio corta el
                    // hilo de fase y deja count por debajo de lo acumulado. En los dos casos el
                    // tramo contiguo arranca de nuevo: se toman las `count` fases que hay.
                    const int expected = std::min(lastCount[i] + fresh,
                                                  wma::analysis::PhaseSlopeEstimator::kMaxWindows);
                    if (windows < lastWindows[i] || fresh > count || count < expected) {
                        segment[i] = static_cast<int>(h.size());
                        for (int k = 0; k < count; ++k) h.push_back(e.regressionPhaseAt(k));
                    } else {
                        for (int k = count - fresh; k < count; ++k) h.push_back(e.regressionPhaseAt(k));
                    }
                    lastWindows[i] = windows;
                    lastCount[i] = count;
                    pub.count[i] = count;
                    pub.histEnd[i] = static_cast<int>(h.size());
                    pub.histSegment[i] = segment[i];
                    pub.measured[i] = s.partialHasMeasurement(i);
                    pub.pCents[i] = pub.measured[i] ? s.partialCents(i) : NAN;
                    pub.pSigma[i] = pub.measured[i] ? s.partialUncertaintyCents(i) : NAN;
                }
                pub.used = s.partialsUsed();
                pub.strobeC = s.cents();
                pub.sigma = s.uncertaintyCents();
                pub.targetHz = s.targetHz();
                const float centsNow = v[wma::analysis::kSnapCents];
                pub.fine = !std::isnan(centsNow);
                if (pub.fine) {
                    pub.centsAbs = 1200.0 * std::log2(pub.targetHz * std::pow(2.0, static_cast<double>(centsNow) / 1200.0) / e.trueHz);
                    double fitB = NAN;
                    pub.admitted = reconstructAdmitted(s, pub.used, pub.strobeC, &fitB);
                }
                o.publicationLog.push_back(pub);
            }
            o.detectedHz = static_cast<double>(v[wma::analysis::kSnapDetectedHz]);
            o.spectralSupport = v[wma::analysis::kSnapSpectralSupport];
            if (o.state == wma::analysis::kStateConverged && o.spectralSupport == 0.0f) {
                ++o.convergedWithoutSupport;
            }
            const float cents = v[wma::analysis::kSnapCents];
            if (!std::isnan(cents)) {
                o.published = true;
                o.lastReadingSec = static_cast<double>(written) / data.sampleRate;
                o.readingState = o.state;
                // REQ-035 S1 — las sondas del strobe, en ESTA publicacion. Se leen despues de
                // `drainOnce()`, en el mismo hilo: es el estado que la publicacion acaba de copiar.
                const wma::analysis::StrobeTracker& s = analysis.strobe();
                for (int i = 0; i < Outcome::kPartials; ++i) {
                    o.partialMeasured[i] = s.partialHasMeasurement(i);
                    o.partialCents[i] = o.partialMeasured[i] ? s.partialCents(i) : NAN;
                    o.partialSigma[i] = o.partialMeasured[i] ? s.partialUncertaintyCents(i) : NAN;
                }
                o.partialsUsed = s.partialsUsed();
                o.strobeC = s.cents();
                o.strobeTargetHz = s.targetHz();
                // En Hz ABSOLUTOS, con el objetivo de ESTA publicacion (el modo rapido puede
                // haberlo movido entre una y otra). Ver la nota de `cents`.
                o.fineHz = o.strobeTargetHz * std::pow(2.0, static_cast<double>(cents) / 1200.0);
                o.fineVsTrueCents = 1200.0 * std::log2(o.fineHz / e.trueHz);
                o.cents = o.fineVsTrueCents;
                o.trajectory.emplace_back(o.lastReadingSec, o.cents);
                o.strobeSigmaC = s.uncertaintyCents();
                o.coarseSeenByStrobe = s.coarseDeviationCents();
                o.snapshotB = static_cast<double>(v[wma::analysis::kSnapInharmonicityB]);
                o.admittedMask = reconstructAdmitted(s, o.partialsUsed, o.strobeC, &o.fitB);
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
