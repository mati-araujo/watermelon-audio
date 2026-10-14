/**
 * LatchInstrument.h — REQ-055 S0. LA VARA: cuanto tarda y cuanto se equivoca el afinador por
 * ENGANCHE.
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
                                  const std::vector<OracleSegment>& oracle, const Params& params) {
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
    }

    int current = -1;   // indice en `openings`
    size_t nextOpening = 0;
    for (const Publication& pub : pubs) {
        while (nextOpening < openings.size() && openings[nextOpening].t0 < pub.pos) current = static_cast<int>(nextOpening++);
        if (current < 0 || latchOf[current] < 0) continue;

        Latch& l = latches[latchOf[current]];
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

}  // namespace wma_test::latch
