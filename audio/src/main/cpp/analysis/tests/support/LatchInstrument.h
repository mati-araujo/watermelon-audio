/**
 * LatchInstrument.h — REQ-055 S0. LA VARA: cuanto tarda y cuanto se equivoca el afinador por
 * ENGANCHE y POR ATAQUE (`measureAttacks`, mas abajo: t0 del oraculo, D11).
 *
 * Es el nucleo PURO del instrumento: recibe la secuencia de publicaciones de una corrida (ya
 * registradas por quien alimento el motor), la serie de rms por VENTANA FIJA de la senal alimentada,
 * los cambios de objetivo y un oraculo instantaneo, y devuelve las cifras por enganche. No lee archivos ni toca el motor; por eso se prueba con
 * secuencias a mano (`test_latch_instrument.cpp`, suite `LatchInstrument`) sin corpus. Quien
 * alimenta el motor y la linea de base commiteada viven en ese mismo test.
 *
 * DEFINICION DE ENGANCHE (acordada con el consumidor; es literal, no se reinterpreta)
 * -----------------------------------------------------------------------------------
 * La «ventana» es FIJA: `kLatchWindowFrames` (4096, la del estimador fino) sobre la senal mono
 * alimentada (0,5·(L+R), la mezcla del ring), en una grilla alineada al frame 0 del flujo e
 * independiente de la tanda con que se drene (`windowLevels`). La DECISION silencio/senal se toma
 * con esa ventana: con el rms del bloque drenado, un bloque de silencio digital al comienzo de cada
 * archivo abria enganches espurios.
 *
 * t_reenganche = cambio de objetivo (el frame exacto del evento), o —con objetivo puesto— el ATAQUE
 * de la primera ventana con rms >= 0,001 que sigue a una ventana con rms < 0,001. El ataque se fecha
 * en el inicio del primer SUB-BLOQUE de `kLatchSubBlockFrames` (256, grilla alineada al frame 0) con
 * rms >= 0,001 dentro de esa ventana, no en el borde de la ventana: un ataque en el frame 8000 tras
 * silencio es t0 = 7936, no 4096 (que inflaba t_primera hasta 85 ms). El enganche dura hasta el
 * siguiente t_reenganche. Un re-punteo con la cuerda sonando NO es un enganche: no hay ventana bajo
 * 0,001 en el medio. Al empezar a alimentar con objetivo puesto, la primera ventana con senal (sin
 * ventana previa) es t_reenganche; si el evento cae en el inicio de esa misma ventana es UN solo
 * enganche, fechado en el evento.
 *
 * 🔴 EL ORACULO ES INSTANTANEO, NO SOSTENIDO. Contra la altura sostenida de la nota, el glide del
 * ataque se lee como error del motor aunque el motor este siguiendo bien a la cuerda (REQ-055 D4).
 * Cada publicacion se compara contra el tramo del oraculo que YA habia terminado cuando se publico
 * —el de `t_fin` mas cercano que no la supera—, nunca contra uno posterior: ese todavia no
 * existia para el motor y compararlo premiaria un adelanto que no puede tener.
 */
#pragma once

#include "../../AnalysisSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace wma_test::latch {

/// La ventana fija del estimador fino, en frames. La unica que usa la definicion de enganche.
inline constexpr int kLatchWindowFrames = 4096;

/// La granularidad con que se fecha el ataque dentro de una ventana con senal, en frames.
inline constexpr int kLatchSubBlockFrames = 256;

/// El piso de senal de la definicion de enganche. Es el `kSilenceFloor` del motor.
inline constexpr double kLatchSignalFloor = 0.001;

/// Una publicacion del analisis, tal como la vio quien alimento el motor.
struct Publication {
    /// `ring.readPosition()` DESPUES de `drainOnce()`: la publicacion se fecha cuando el motor la dio.
    int64_t pos = 0;
    int state = -1;
    /// Altura publicada en Hz absolutos: `targetHz · 2^(cents/1200)`. NaN = sin lectura fina.
    double hz = std::numeric_limits<double>::quiet_NaN();
    double sigmaCents = std::numeric_limits<double>::quiet_NaN();
    // REQ-058 S1 (D-7): las sondas del strobe en esta publicacion, que llena quien alimenta el motor.
    int partials = 0;            ///< parciales usados (`partialsUsed()`); 0 sin lectura
    bool early = false;          ///< la lectura salio del camino temprano (`readingIsEarly()`)
    /// Frame del RING en que empezo el segmento vigente del motor (`segmentStartFrame()` llevado al ring).
    /// -1 = desconocido: ninguna publicacion cuenta como vuelta.
    int64_t segmentStart = -1;
};

/// El rms de la ventana de `kLatchWindowFrames` que empieza en `start` (frames del flujo alimentado).
struct Window {
    int64_t start = 0;
    double rms = 0.0;
    /// Inicio del primer sub-bloque con senal dentro de la ventana (donde se fecha t0). -1 = no
    /// calculado: la ventana se fecha en `start`.
    int64_t onset = -1;
    int64_t t0() const { return onset >= 0 ? onset : start; }
};

/// `AnalysisThread::setTargetHz` en `pos` (frames del ring). `hasTarget = false` = se quito el objetivo.
/// Aplica a la primera publicacion con `pos` posterior: el motor lo toma al empezar el drenado.
struct TargetEvent {
    int64_t pos = 0;
    bool hasTarget = true;
};

/// Un tramo del oraculo instantaneo, en segundos desde el inicio de la alimentacion.
struct OracleSegment {
    double tIni = 0.0;
    double tFin = 0.0;
    double hz = std::numeric_limits<double>::quiet_NaN();   ///< NaN = el oraculo no declara altura
};

struct Params {
    int sampleRate = 48000;
    double signalFloor = kLatchSignalFloor;
};

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Agrega al flujo mono la mezcla del ring (`0,5f·(l + r)`, en float) de `frames` frames estereo.
inline void appendMonoMix(std::vector<float>* out, const float* interleaved, int frames) {
    for (int i = 0; i < frames; ++i) out->push_back(0.5f * (interleaved[2 * i] + interleaved[2 * i + 1]));
}

/// El rms de cada ventana COMPLETA de `windowFrames`, en la grilla que arranca en el frame 0 del
/// flujo, y el inicio de su primer sub-bloque de `subBlockFrames` con rms >= `signalFloor`. Una cola
/// incompleta no es ventana: no se mide con menos muestras que las otras. `windowFrames` debe ser
/// multiplo de `subBlockFrames`: asi la ventana con senal siempre tiene algun sub-bloque con senal.
inline std::vector<Window> windowLevels(const std::vector<float>& mono, int windowFrames,
                                        int subBlockFrames = kLatchSubBlockFrames,
                                        double signalFloor = kLatchSignalFloor) {
    std::vector<Window> out;
    for (size_t s = 0; s + windowFrames <= mono.size(); s += windowFrames) {
        double acc = 0.0;
        for (int i = 0; i < windowFrames; ++i) acc += static_cast<double>(mono[s + i]) * mono[s + i];
        Window w;
        w.start = static_cast<int64_t>(s);
        w.rms = std::sqrt(acc / windowFrames);
        w.onset = w.start;
        for (int b = 0; b + subBlockFrames <= windowFrames; b += subBlockFrames) {
            double sub = 0.0;
            for (int i = 0; i < subBlockFrames; ++i) sub += static_cast<double>(mono[s + b + i]) * mono[s + b + i];
            if (std::sqrt(sub / subBlockFrames) >= signalFloor) {
                w.onset = w.start + b;
                break;
            }
        }
        out.push_back(w);
    }
    return out;
}

/// Un corte del volcado ubicado en el archivo (`# corte NOMBRE INI FIN PERDIDOS` del oraculo), en frames,
/// intervalo CERRADO: una perdida es ini == fin == el frame donde falta; una costura [fileFrame, fileFrame + span].
struct FrameCut {
    int64_t ini = 0;
    int64_t fin = 0;
};

/// La ventana del detector del oraculo: la guarda de un ataque empieza 2048 frames antes de su t0, porque
/// el detector mira 2048 antes y 2048 despues. Es LA MISMA regla que usa el oraculo; no se cambia aca.
inline constexpr int64_t kCutGuardFrames = 2048;

/// Cierto si algun corte toca la guarda [t0 - 2048, tEnd] (cerrada) de un ataque o enganche.
inline bool cutTouchesSpan(const std::vector<FrameCut>& cuts, int64_t t0, int64_t tEnd) {
    for (const FrameCut& c : cuts) {
        if (c.ini <= tEnd && c.fin >= t0 - kCutGuardFrames) return true;
    }
    return false;
}

struct Latch {
    enum class Cause { kTargetChange, kSignalAfterSilence };
    Cause cause = Cause::kTargetChange;
    int64_t t0Frames = 0;
    double t0Sec = 0.0;

    int publications = 0;        ///< todas las del enganche, con o sin lectura fina
    int readings = 0;            ///< las que traen cents
    /// Desde t0 hasta la primera publicacion con cents / con CONVERGED. -1 / NaN = no llego.
    int64_t firstReadingFrames = -1;
    double firstReadingSec = kNaN;
    int64_t convergedFrames = -1;
    double convergedSec = kNaN;

    int withError = 0;           ///< lecturas con oraculo: las que tienen error
    int noOracle = 0;            ///< lecturas SIN oraculo (sin tramo previo, o tramo sin hz): aparte
    int covered = 0;             ///< de `withError`, las de |error| <= 2σ
    double firstErrorCents = kNaN;   ///< error con signo de la primera lectura (NaN si no tuvo oraculo)
    double firstSigmaCents = kNaN;
    double medianAbsCents = kNaN;
    double p90AbsCents = kNaN;
    double maxAbsCents = kNaN;
    double coverage = kNaN;      ///< covered / withError
    std::vector<double> absErrors;   ///< |error| de cada lectura con oraculo, para agregar entre enganches
    /// Lo toca un corte: sale SIN cifras (todo en cero/-1/NaN) y quien agrega debe contarlo aparte.
    bool excludedByCut = false;
};

/// Mediana de un vector (promedio de los dos centrales si n es par). NaN si esta vacio.
inline double median(std::vector<double> v) {
    if (v.empty()) return kNaN;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// Percentil por rango mas cercano: el elemento ceil(p·n) (1-based). Reproducible a mano.
inline double percentile(std::vector<double> v, double p) {
    if (v.empty()) return kNaN;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(n)));
    rank = std::min(std::max<size_t>(rank, 1), n);
    return v[rank - 1];
}

/// El tramo cuyo `t_fin` es el mayor que no supera `tPub`. nullptr si ninguno habia terminado.
inline const OracleSegment* segmentFor(const std::vector<OracleSegment>& oracle, double tPub) {
    const OracleSegment* best = nullptr;
    for (const OracleSegment& s : oracle) {
        if (s.tFin <= tPub && (best == nullptr || s.tFin > best->tFin)) best = &s;
    }
    return best;
}

namespace detail {

inline Latch openLatch(Latch::Cause cause, int64_t t0, const Params& p) {
    Latch l;
    l.cause = cause;
    l.t0Frames = t0;
    l.t0Sec = static_cast<double>(t0) / p.sampleRate;
    return l;
}

inline void finish(Latch& l) {
    l.medianAbsCents = median(l.absErrors);
    l.p90AbsCents = percentile(l.absErrors, 0.9);
    l.maxAbsCents = l.absErrors.empty() ? kNaN : *std::max_element(l.absErrors.begin(), l.absErrors.end());
    l.coverage = l.withError > 0 ? static_cast<double>(l.covered) / l.withError : kNaN;
}

}  // namespace detail

/**
 * Las cifras por enganche de una corrida. `events` va ordenado por `pos`, `pubs` por `pos`,
 * `windows` por `start`.
 *
 * Los t0 salen SOLO de los eventos y de las ventanas: las publicaciones se atribuyen despues al
 * ultimo enganche con t0 < pos, asi que ni la cantidad de enganches ni sus t0 dependen de la
 * granularidad con que se publico. Un evento con ventana de por medio vacia es un enganche sin
 * lecturas. Sin objetivo puesto (ningun evento, o el ultimo con `hasTarget = false`) no hay enganche
 * y las publicaciones no se atribuyen a nadie.
 */
inline std::vector<Latch> measure(const std::vector<Publication>& pubs, const std::vector<Window>& windows,
                                  const std::vector<TargetEvent>& events,
                                  const std::vector<OracleSegment>& oracle, const Params& params,
                                  const std::vector<FrameCut>& cuts = {}, int64_t streamEndFrames = -1) {
    struct Opening {
        int64_t t0;
        Latch::Cause cause;
        bool open;   ///< false = se quito el objetivo
    };
    std::vector<Opening> openings;
    size_t nextEvent = 0;
    bool targetSet = false;
    bool prevSilent = true;   // sin ventana previa: la primera ventana con senal abre enganche

    const auto applyEvent = [&](const TargetEvent& ev) {
        targetSet = ev.hasTarget;
        openings.push_back({ev.pos, Latch::Cause::kTargetChange, targetSet});
    };
    for (const Window& w : windows) {
        // Un evento en el instante de la ventana va ANTES: si ademas tiene senal, es un solo enganche.
        while (nextEvent < events.size() && events[nextEvent].pos <= w.start) applyEvent(events[nextEvent++]);
        const bool signal = w.rms >= params.signalFloor;
        if (signal && prevSilent) {
            // Los eventos anteriores al ataque van antes que su enganche: `openings` queda ordenada por t0.
            const int64_t attack = w.t0();
            while (nextEvent < events.size() && events[nextEvent].pos <= attack) applyEvent(events[nextEvent++]);
            const bool sameInstant = !openings.empty() && openings.back().open &&
                                     (openings.back().t0 == w.start || openings.back().t0 == attack);
            if (targetSet && !sameInstant) openings.push_back({attack, Latch::Cause::kSignalAfterSilence, true});
        }
        prevSilent = !signal;
    }
    while (nextEvent < events.size()) applyEvent(events[nextEvent++]);

    std::vector<Latch> latches;
    std::vector<int> latchOf(openings.size(), -1);
    for (size_t i = 0; i < openings.size(); ++i) {
        if (!openings[i].open) continue;
        latchOf[i] = static_cast<int>(latches.size());
        latches.push_back(detail::openLatch(openings[i].cause, openings[i].t0, params));
        // El enganche dura hasta la siguiente apertura (o el final del flujo; sin el, la ultima publicacion).
        int64_t end = streamEndFrames >= 0 ? streamEndFrames : (pubs.empty() ? openings[i].t0 : pubs.back().pos);
        if (i + 1 < openings.size()) end = openings[i + 1].t0;
        latches.back().excludedByCut = cutTouchesSpan(cuts, openings[i].t0, end);
    }

    int current = -1;   // indice en `openings`
    size_t nextOpening = 0;
    for (const Publication& pub : pubs) {
        while (nextOpening < openings.size() && openings[nextOpening].t0 < pub.pos) current = static_cast<int>(nextOpening++);
        if (current < 0 || latchOf[current] < 0) continue;

        Latch& l = latches[latchOf[current]];
        if (l.excludedByCut) continue;
        ++l.publications;
        const bool hasReading = !std::isnan(pub.hz);
        if (hasReading) {
            ++l.readings;
            if (l.firstReadingFrames < 0) {
                l.firstReadingFrames = pub.pos - l.t0Frames;
                l.firstReadingSec = static_cast<double>(l.firstReadingFrames) / params.sampleRate;
            }
        }
        if (pub.state == wma::analysis::kStateConverged && l.convergedFrames < 0) {
            l.convergedFrames = pub.pos - l.t0Frames;
            l.convergedSec = static_cast<double>(l.convergedFrames) / params.sampleRate;
        }
        if (!hasReading) continue;

        const bool firstReading = l.readings == 1;
        if (firstReading) l.firstSigmaCents = pub.sigmaCents;
        const OracleSegment* seg = segmentFor(oracle, static_cast<double>(pub.pos) / params.sampleRate);
        if (seg == nullptr || !(seg->hz > 0.0) || !std::isfinite(seg->hz)) {
            ++l.noOracle;
            continue;
        }
        const double err = 1200.0 * std::log2(pub.hz / seg->hz);
        if (firstReading) l.firstErrorCents = err;
        ++l.withError;
        l.absErrors.push_back(std::fabs(err));
        if (std::fabs(err) <= 2.0 * pub.sigmaCents) ++l.covered;   // σ NaN: la comparacion es falsa
    }
    for (Latch& l : latches) detail::finish(l);
    return latches;
}

// =====================================================================================
// POR ATAQUE (REQ-055 D11, AC-055.2)
// =====================================================================================
//
// La definicion de enganche no se cumple en un telefono: la sala del g42 esta a -48,5 dBFS y ninguna
// ventana baja de 0,001, asi que un re-punteo nunca abre enganche. Por eso se mide ademas POR ATAQUE:
//
//   · t0 de cada ataque lo da el ORACULO (`scripts/corpus-reference-pitch.py`, lineas `# ataque` del
//     TSV corto), NUNCA el motor (R-API-48). Aca es un dato de entrada: ni las publicaciones ni el
//     nivel lo mueven.
//   · El ataque i dura (t0_i, t0_{i+1}] (o hasta el final del flujo): una publicacion con pos == t0
//     es ANTERIOR (el motor no habia visto ninguna muestra del ataque) y una con pos == t0_{i+1} es
//     del ataque i (todavia no vio el siguiente).
//   · `t_primera` = primera publicacion con cents (no NaN) DESPUES de t0; `t_conv` = primer CONVERGED
//     despues de t0. Si la ultima publicacion con pos <= t0 ya traia cents (o CONVERGED), el ataque
//     viene ARRASTRADO: el motor no solto la lectura de antes. Se marca, no se descarta; un `t_primera`
//     arrastrado no es una latencia del ataque.
//   · El error de cada publicacion es contra el oraculo instantaneo: el tramo de `t_fin` mas cercano
//     que no supera la publicacion, ENTRE LOS QUE EMPIEZAN EN t0 O DESPUES (`attackSegmentFor`). Un tramo
//     que arranca antes de t0 mezcla la nota anterior con el ataque; sin tramo propio terminado la
//     publicacion cuenta como sin oraculo.
//   · Error a 250 ms y a 1 s: la lectura VISIBLE en t0 + Δ (la ultima publicacion en (t0, t0 + Δ]),
//     contra el tramo propio del ataque ya terminado en t0 + Δ. Sin publicacion propia del ataque, o con NaN, no hay
//     lectura (NaN, contada aparte); si t0 + Δ cae despues del final del ataque, no aplica (NaN).
//
// Nada de esto toca el motor: es aritmetica sobre lo que el motor ya publico.

/// Los instantes desde el ataque en que se lee el error visible, en segundos.
inline constexpr double kAttackProbeEarlySec = 0.25;
inline constexpr double kAttackProbeLateSec = 1.0;

/// La lectura visible en t0 + Δ.
struct Probe {
    bool applies = false;   ///< t0 + Δ cae dentro del ataque
    bool reading = false;   ///< habia una publicacion propia del ataque con cents
    bool oracle = false;    ///< habia un tramo del oraculo propio del ataque, terminado, con hz
    double errorCents = kNaN;
    double sigmaCents = kNaN;
    bool covered = false;   ///< |error| <= 2σ
};

/// REQ-058 S1 — una publicacion con cents y oraculo, juzgada (insumo de los estratos de D21).
struct JudgedPublication {
    double sinceT0Sec = 0.0;   ///< (pos − t0)/rate: la FASE de la nota
    int partials = 0;
    bool early = false;
    int state = -1;
    double errorCents = kNaN;
    double sigmaCents = kNaN;
    bool covered = false;      ///< |error| <= 2σ
};

/// La holgura con que un segmento del motor cuenta como abierto EN el ataque: el detector del motor fecha
/// en bloques de 1024 alineados a su rejilla y el del oraculo en la grilla de 128 (D-7).
inline constexpr int64_t kReturnSegmentSlackFrames = 1024;

/// AC-058.11 b — desde cuando se cuenta la cobertura en el tiempo, en segundos desde t0.
inline constexpr double kTimeCoverageFromSec = 0.25;

struct Attack {
    int64_t t0Frames = 0;
    double t0Sec = 0.0;
    int64_t endFrames = 0;   ///< el final del ataque: el siguiente t0 o el final del flujo
    bool readingCarried = false;     ///< la ultima publicacion <= t0 traia cents
    bool convergedCarried = false;   ///< la ultima publicacion <= t0 era CONVERGED

    int publications = 0;
    int readings = 0;
    int64_t firstReadingFrames = -1;
    double firstReadingSec = kNaN;
    int64_t convergedFrames = -1;
    double convergedSec = kNaN;

    int withError = 0;
    int noOracle = 0;
    int covered = 0;
    double firstErrorCents = kNaN;
    double firstSigmaCents = kNaN;
    double medianAbsCents = kNaN;
    double p90AbsCents = kNaN;
    double maxAbsCents = kNaN;
    double coverage = kNaN;
    std::vector<double> absErrors;

    /// Lo toca un corte: sale SIN cifras y quien agrega debe contarlo aparte, no sumarlo.
    bool excludedByCut = false;

    Probe early;   ///< a kAttackProbeEarlySec
    Probe late;    ///< a kAttackProbeLateSec

    // ---- REQ-058 S1 (D-7) ----
    /// `t_vuelta`: la primera publicacion con cents DEL SEGMENTO ABIERTO EN ESTE ATAQUE (su
    /// `segmentStart >= t0 − kReturnSegmentSlackFrames`). Una lectura arrastrada del segmento viejo no
    /// cuenta: es lo que `t_primera` del re-punteo mide hoy (0,003 s) y lo que no deja ver AC-058.7.
    int64_t returnFrames = -1;
    double returnSec = kNaN;
    /// Lo mismo para `CONVERGED`: la primera del segmento abierto en el ataque (AC-058.7: las 12 ventanas se
    /// cuentan acumuladas entre segmentos compatibles, asi que vuelve antes que desde cero).
    int64_t returnConvergedFrames = -1;
    /// AC-058.11 b — publicaciones pasados `kTimeCoverageFromSec` del ataque mientras el oraculo declara
    /// altura (un tramo PROPIO del ataque, terminado, con hz), y cuantas de esas traen cents.
    int soundingPublications = 0;
    int soundingWithCents = 0;
    /// Las publicaciones con cents y oraculo, juzgadas una por una: el insumo de los ESTRATOS (D21).
    std::vector<JudgedPublication> judged;
};

namespace detail {

/// Como `segmentFor`, pero sólo entre los tramos que EMPIEZAN en el ataque o despues (`t_ini >= t0`).
/// Un tramo que arranca antes de t0 mezcla la cola de la nota anterior con el ataque (en corpus-v1 el
/// primer tramo de varios archivos da -39..-143 c): juzgar al motor contra él le carga el error del
/// oraculo. Sin tramo propio terminado, la publicacion queda sin oraculo (`noOracle`).
inline const OracleSegment* attackSegmentFor(const std::vector<OracleSegment>& oracle, double t0Sec, double tPub) {
    const OracleSegment* best = nullptr;
    for (const OracleSegment& s : oracle) {
        if (s.tIni >= t0Sec && s.tFin <= tPub && (best == nullptr || s.tFin > best->tFin)) best = &s;
    }
    return best;
}

inline Probe probeAt(const std::vector<Publication>& pubs, const std::vector<OracleSegment>& oracle,
                     const Attack& a, double deltaSec, int rate) {
    Probe p;
    const int64_t at = a.t0Frames + static_cast<int64_t>(std::llround(deltaSec * rate));
    if (at > a.endFrames) return p;
    p.applies = true;
    const Publication* visible = nullptr;
    for (const Publication& pub : pubs) {
        if (pub.pos <= a.t0Frames) continue;
        if (pub.pos > at) break;
        visible = &pub;
    }
    if (visible == nullptr || std::isnan(visible->hz)) return p;
    p.reading = true;
    p.sigmaCents = visible->sigmaCents;
    const OracleSegment* seg = attackSegmentFor(oracle, a.t0Sec, static_cast<double>(at) / rate);
    if (seg == nullptr || !(seg->hz > 0.0) || !std::isfinite(seg->hz)) return p;
    p.oracle = true;
    p.errorCents = 1200.0 * std::log2(visible->hz / seg->hz);
    p.covered = std::fabs(p.errorCents) <= 2.0 * visible->sigmaCents;
    return p;
}

}  // namespace detail

/**
 * Las cifras por ataque. `pubs` ordenadas por `pos`; `attackFrames` (del ORACULO) ordenados y
 * dentro de [0, streamEndFrames); `streamEndFrames` = frames alimentados. Un ataque cuya guarda toca un
 * corte (`cutTouchesSpan`) vuelve con `excludedByCut` y sin cifras.
 */
inline std::vector<Attack> measureAttacks(const std::vector<Publication>& pubs, const std::vector<int64_t>& attackFrames,
                                          const std::vector<OracleSegment>& oracle, int64_t streamEndFrames,
                                          const Params& params, const std::vector<FrameCut>& cuts = {}) {
    std::vector<Attack> out;
    for (size_t i = 0; i < attackFrames.size(); ++i) {
        Attack a;
        a.t0Frames = attackFrames[i];
        a.t0Sec = static_cast<double>(a.t0Frames) / params.sampleRate;
        a.endFrames = i + 1 < attackFrames.size() ? attackFrames[i + 1] : streamEndFrames;
        if (cutTouchesSpan(cuts, a.t0Frames, a.endFrames)) {
            a.excludedByCut = true;
            out.push_back(std::move(a));
            continue;
        }
        const Publication* before = nullptr;
        for (const Publication& pub : pubs) {
            if (pub.pos <= a.t0Frames) {
                before = &pub;
                continue;
            }
            if (pub.pos > a.endFrames) break;
            ++a.publications;
            const bool hasReading = !std::isnan(pub.hz);
            const double sinceT0 = static_cast<double>(pub.pos - a.t0Frames) / params.sampleRate;
            const bool ownSegment = pub.segmentStart >= 0 && pub.segmentStart >= a.t0Frames - kReturnSegmentSlackFrames;
            if (hasReading && a.returnFrames < 0 && ownSegment) {
                a.returnFrames = pub.pos - a.t0Frames;
                a.returnSec = static_cast<double>(a.returnFrames) / params.sampleRate;
            }
            if (hasReading && ownSegment && a.returnConvergedFrames < 0 && pub.state == wma::analysis::kStateConverged)
                a.returnConvergedFrames = pub.pos - a.t0Frames;
            if (sinceT0 > kTimeCoverageFromSec) {
                const OracleSegment* own = detail::attackSegmentFor(oracle, a.t0Sec, static_cast<double>(pub.pos) / params.sampleRate);
                if (own != nullptr && own->hz > 0.0 && std::isfinite(own->hz)) {
                    ++a.soundingPublications;
                    if (hasReading) ++a.soundingWithCents;
                }
            }
            if (pub.state == wma::analysis::kStateConverged && a.convergedFrames < 0) {
                a.convergedFrames = pub.pos - a.t0Frames;
                a.convergedSec = static_cast<double>(a.convergedFrames) / params.sampleRate;
            }
            if (!hasReading) continue;
            ++a.readings;
            const bool first = a.readings == 1;
            if (first) {
                a.firstReadingFrames = pub.pos - a.t0Frames;
                a.firstReadingSec = static_cast<double>(a.firstReadingFrames) / params.sampleRate;
                a.firstSigmaCents = pub.sigmaCents;
            }
            const OracleSegment* seg = detail::attackSegmentFor(oracle, a.t0Sec, static_cast<double>(pub.pos) / params.sampleRate);
            if (seg == nullptr || !(seg->hz > 0.0) || !std::isfinite(seg->hz)) {
                ++a.noOracle;
                continue;
            }
            const double err = 1200.0 * std::log2(pub.hz / seg->hz);
            if (first) a.firstErrorCents = err;
            ++a.withError;
            a.absErrors.push_back(std::fabs(err));
            if (std::fabs(err) <= 2.0 * pub.sigmaCents) ++a.covered;   // σ NaN: la comparacion es falsa
            a.judged.push_back({sinceT0, pub.partials, pub.early, pub.state, err, pub.sigmaCents,
                                std::fabs(err) <= 2.0 * pub.sigmaCents});
        }
        a.readingCarried = before != nullptr && !std::isnan(before->hz);
        a.convergedCarried = before != nullptr && before->state == wma::analysis::kStateConverged;
        a.medianAbsCents = median(a.absErrors);
        a.p90AbsCents = percentile(a.absErrors, 0.9);
        a.maxAbsCents = a.absErrors.empty() ? kNaN : *std::max_element(a.absErrors.begin(), a.absErrors.end());
        a.coverage = a.withError > 0 ? static_cast<double>(a.covered) / a.withError : kNaN;
        a.early = detail::probeAt(pubs, oracle, a, kAttackProbeEarlySec, params.sampleRate);
        a.late = detail::probeAt(pubs, oracle, a, kAttackProbeLateSec, params.sampleRate);
        out.push_back(std::move(a));
    }
    return out;
}

/// El rango de un tiempo (frames desde t0) sobre varias corridas del mismo enganche. Las corridas donde no
/// llego (-1) NO entran a min/max: se cuentan en `missing`. Sin ninguna que llegue, min = max = -1.
struct FrameRange {
    int64_t min = -1;
    int64_t max = -1;
    int missing = 0;
};

inline FrameRange frameRange(const std::vector<int64_t>& framesPerRun) {
    FrameRange r;
    for (int64_t f : framesPerRun) {
        if (f < 0) {
            ++r.missing;
            continue;
        }
        r.min = r.min < 0 ? f : std::min(r.min, f);
        r.max = std::max(r.max, f);
    }
    return r;
}

/// Los errores de VARIOS enganches juntos (todas las publicaciones, no el promedio de medianas).
struct Pooled {
    int withError = 0;
    int covered = 0;
    double medianAbsCents = kNaN;
    double p90AbsCents = kNaN;
    double maxAbsCents = kNaN;
    double coverage = kNaN;
};

inline Pooled poolLatchErrors(const std::vector<Latch>& latches) {
    Pooled out;
    std::vector<double> all;
    for (const Latch& l : latches) {
        all.insert(all.end(), l.absErrors.begin(), l.absErrors.end());
        out.withError += l.withError;
        out.covered += l.covered;
    }
    out.medianAbsCents = median(all);
    out.p90AbsCents = percentile(all, 0.9);
    out.maxAbsCents = all.empty() ? kNaN : *std::max_element(all.begin(), all.end());
    out.coverage = out.withError > 0 ? static_cast<double>(out.covered) / out.withError : kNaN;
    return out;
}

// =====================================================================================
// REQ-058 S1 (D-7): ESTRATOS, POR CUERDA Y NO INFLACION
// =====================================================================================

/// La fase de la nota (D21): 0 = (0, 0,25] s, 1 = (0,25, 1] s, 2 = asentada (> 1 s). Los bordes van
/// con la fase ANTERIOR: una publicacion a 0,25 s exactos es del ataque.
inline int phaseStratum(double sinceT0Sec) { return sinceT0Sec <= 0.25 ? 0 : (sinceT0Sec <= 1.0 ? 1 : 2); }

/// Los parciales usados como estrato (D21): 1, 2 o 3 (= «3 o mas»).
inline int partialsStratum(int partials) { return partials <= 1 ? 1 : (partials == 2 ? 2 : 3); }

/// Una celda de estrato: cobertura de 2σ y σ/|error|.
struct CoverageCell {
    int n = 0;
    int covered = 0;
    std::vector<double> ratio;   ///< σ/|error| (sin los |error| == 0)
    void add(const JudgedPublication& j) {
        ++n;
        if (j.covered) ++covered;
        if (std::fabs(j.errorCents) > 0.0) ratio.push_back(j.sigmaCents / std::fabs(j.errorCents));
    }
    double coverage() const { return n > 0 ? static_cast<double>(covered) / n : kNaN; }
    double medianRatio() const { return median(ratio); }
};

/// D20/D47: con n <= 5 ataques por cuerda (corpus-v1 tiene UNO por archivo y pasada) un p90 es el maximo
/// y un 95 % es todo-o-nada. Lo imprime quien reporta por cuerda, al lado de la cifra.
inline constexpr int kDegenerateStringAttacks = 5;
inline const char* degenerateStringWarning(int attacks) {
    return attacks <= kDegenerateStringAttacks ? " (n <= 5: el p90 es el maximo y el 95 % es todo-o-nada)" : "";
}

}  // namespace wma_test::latch
