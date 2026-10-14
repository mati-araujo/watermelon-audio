/**
 * LatchInstrument.h — REQ-055 S0. LA VARA: cuanto tarda y cuanto se equivoca el afinador por
 * ENGANCHE.
 *
 * Es el nucleo PURO del instrumento: recibe la secuencia de publicaciones de una corrida (ya
 * registradas por quien alimento el motor), los cambios de objetivo y un oraculo instantaneo, y
 * devuelve las cifras por enganche. No lee archivos ni toca el motor; por eso se prueba con
 * secuencias a mano (`test_latch_instrument.cpp`, suite `LatchInstrument`) sin corpus. Quien
 * alimenta el motor y la linea de base commiteada viven en ese mismo test.
 *
 * DEFINICION DE ENGANCHE (acordada con el consumidor; es literal, no se reinterpreta)
 * -----------------------------------------------------------------------------------
 * t_reenganche = cambio de objetivo, o (con objetivo puesto) la primera ventana con senal
 * (rms >= 0,001 del bloque mono drenado) despues de una ventana con rms < 0,001. El enganche dura
 * hasta el siguiente t_reenganche. Un re-punteo con la cuerda sonando NO es un enganche: no hay
 * ventana bajo 0,001 en el medio. Al empezar a alimentar con objetivo puesto y ring vacio, la
 * primera ventana con senal es t_reenganche.
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

/// Una publicacion del analisis, tal como la vio quien alimento el motor.
struct Publication {
    /// `ring.readPosition()` ANTES y DESPUES de `drainOnce()`: el bloque drenado es
    /// [posStart, pos). La publicacion se fecha en `pos` (cuando el motor la dio); el INICIO de la
    /// ventana con senal —`t0` de un enganche por senal— es `posStart`.
    int64_t posStart = 0;
    int64_t pos = 0;
    int state = -1;
    /// Altura publicada en Hz absolutos: `targetHz · 2^(cents/1200)`. NaN = sin lectura fina.
    double hz = std::numeric_limits<double>::quiet_NaN();
    double sigmaCents = std::numeric_limits<double>::quiet_NaN();
    double rms = 0.0;   ///< `kSnapLevelRms`: el rms del bloque drenado
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
    /// El piso de senal de la definicion de enganche. Es el `kSilenceFloor` del motor.
    double signalFloor = 0.001;
};

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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
 * Las cifras por enganche de una corrida. `events` va ordenado por `pos`, `pubs` por `pos`.
 *
 * Un evento sin publicacion posterior no abre enganche: no hay nada que medir en el.
 * Sin objetivo puesto (ningun evento, o el ultimo con `hasTarget = false`) no hay enganche y las
 * publicaciones no se atribuyen a nadie.
 */
inline std::vector<Latch> measure(const std::vector<Publication>& pubs,
                                  const std::vector<TargetEvent>& events,
                                  const std::vector<OracleSegment>& oracle, const Params& params) {
    std::vector<Latch> latches;
    size_t nextEvent = 0;
    bool targetSet = false;
    bool prevSilent = true;   // "ring vacio al empezar": la primera ventana con senal abre enganche
    bool open = false;

    for (const Publication& pub : pubs) {
        while (nextEvent < events.size() && events[nextEvent].pos < pub.pos) {
            const TargetEvent& ev = events[nextEvent++];
            targetSet = ev.hasTarget;
            open = targetSet;
            if (targetSet) latches.push_back(detail::openLatch(Latch::Cause::kTargetChange, ev.pos, params));
        }

        const bool signal = pub.rms >= params.signalFloor;
        // Un cambio de objetivo y la primera ventana con senal en el MISMO instante son un solo
        // enganche (el caso normal al empezar a alimentar); en instantes distintos son dos.
        if (targetSet && signal && prevSilent && !(open && latches.back().t0Frames == pub.posStart)) {
            latches.push_back(detail::openLatch(Latch::Cause::kSignalAfterSilence, pub.posStart, params));
            open = true;
        }
        prevSilent = !signal;
        if (!targetSet || !open) continue;

        Latch& l = latches.back();
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

/// Los errores de VARIOS enganches juntos (todas las publicaciones, no el promedio de medianas).
struct Pooled {
    int withError = 0;
    int covered = 0;
    double medianAbsCents = kNaN;
    double p90AbsCents = kNaN;
    double maxAbsCents = kNaN;
    double coverage = kNaN;
};

inline Pooled pool(const std::vector<Latch>& latches) {
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
