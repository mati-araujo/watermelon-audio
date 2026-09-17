#pragma once

/**
 * @file TrackAnalysis.h
 * @brief Las dos series OFFLINE de una pista para el video de NoisyPad (REQ-043 S1):
 *        pitch por hop (WV-3.2) y envolvente RMS decimada (WV-3.1).
 *
 * QUE ES ESTO Y QUE NO ES
 * -----------------------
 * Logica PURA sobre un buffer mono ya copiado: ni lee el `TrackBuffer`, ni sabe de
 * threads, ni asigna mas que sus propios scratch. La lectura consistente del buffer
 * —cuando es seguro leer, que region, con que rate— es de `AudioLooper`, que es el
 * borde. Asi la aritmetica del eje (donde cae cada `frame`, cuantos puntos hay) se
 * puede afirmar sola, y el looper solo cablea.
 *
 * NO ES RT: corre en el thread de UI/IO, como `TrackBuffer::detectOnsets`.
 *
 * EL EJE (decisiones 6 y 10 de la spec, y R-API-60/61 cuando se archiven)
 * -----------------------------------------------------------------------
 * Pitch: `frame` apunta al CENTRO de la ventana. Sobre un chirp lento la NSDF estima
 * ≈ la f del centro; sellarlo al inicio sesga +1,4 % a 1 oct/s y viola AC-043.1 por
 * construccion. Las ventanas van ENTERAS dentro de la region: el primer punto esta en
 * `regionStart + W/2` y el ultimo en `≤ regionEnd − W/2`; no hay relleno con ceros
 * (fabrica un 0/0 del instrumento) ni envolver al dar la vuelta (eso es `layerFor`,
 * del consumidor). `hopFrames` es exacto: hop < W es solapamiento, no un minimo.
 *
 * Envolvente: ventana = hop, `firstFrame = regionStart`, `bins = floor(region/hop)`.
 * La cola menor que un hop NO tiene bin: un bin parcial tendria otra ventana sin
 * nombre. A 4 bins/s se descartan hasta 250 ms de cola, y a 100 bins/s hasta 10 ms.
 *
 * 0/0 (decision 7): `freqHz = 0 ∧ confidence = 0` EXACTOS cuando la ventana esta bajo
 * el piso (0,001 RMS), la claridad NSDF no llega a 0,5, o la altura no tiene soporte
 * espectral (`SpectralSupportProbe` sobre la MISMA W, −25 dB: rechaza el subarmonico
 * con claridad alta, REQ-031). SIN `AbsenceGate`: la histeresis temporal resuelve el
 * parpadeo del vivo, y offline correria la frontera del silencio N × hop.
 */

#include "../analysis/SpectralSupportProbe.h"
#include "../dsp/McLeodPitch.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace wma::track_analysis {

/// El preset de VOZ, fijado por el motor y no expuesto (decision 5). Entre 40 y 30 ms
/// decidio el tramo de 65 Hz del fixture (AC-043.2, medido en S1): con 30 ms la regla
/// `τmax ≤ W/2` del MPM no baja de 66,7 Hz y el tramo sale clavado en ese techo, a 3,4 %
/// (40 ms: 0,12 %). 60 y no 80 Hz porque un bajo a 65 Hz con rango 80 no sale "sin
/// pitch": sale como su H2 con claridad alta.
constexpr double kVoiceWindowMs = 40.0;
constexpr double kVoiceMinHz = 60.0;
constexpr double kVoiceMaxHz = 1200.0;
/// El umbral de REQ-031, sin cambios: por debajo la altura no esta en la señal.
constexpr double kSupportFloorDb = -25.0;

/// `W = round(40 ms · sr)`. A 48 kHz, 1920.
inline int voiceWindowFrames(int sampleRate) {
    return static_cast<int>(std::lround(kVoiceWindowMs * sampleRate / 1000.0));
}

/// `hopFrames = round(hopMs · sr / 1000)`, redondeado UNA vez. ≤ 0 si no hay hop.
inline int hopFramesForMs(double hopMs, int sampleRate) {
    if (!(hopMs > 0.0) || sampleRate <= 0) return 0;
    return static_cast<int>(std::lround(hopMs * sampleRate / 1000.0));
}

/// `hopFrames = round(sr / binsPerSecond)`, redondeado UNA vez. ≤ 0 si no hay tasa.
inline int hopFramesForBinsPerSecond(double binsPerSecond, int sampleRate) {
    if (!(binsPerSecond > 0.0) || sampleRate <= 0) return 0;
    return static_cast<int>(std::lround(static_cast<double>(sampleRate) / binsPerSecond));
}

/// Cuantos puntos de pitch tiene una region de `regionFrames` con ventana `W` y hop `hop`.
inline int pitchPointCount(int regionFrames, int windowFrames, int hopFrames) {
    if (regionFrames < windowFrames || windowFrames <= 0 || hopFrames <= 0) return 0;
    return (regionFrames - windowFrames) / hopFrames + 1;
}

/**
 * @brief La serie de pitch de una region mono, con `frame` ABSOLUTO (`regionStart` +
 *        centro de ventana). Escribe hasta `maxPoints`; devuelve cuantos escribio.
 *
 * @param mono          `regionFrames` frames, la region entera (solo lectura).
 * @param regionStart   frame absoluto del primer frame de `mono` (= loopStart).
 * @param windowFrames  W, en frames de entrada (`voiceWindowFrames(sr)` en produccion;
 *                      el test de AC-043.2 pasa 30 y 40 ms).
 * @param outFrames/outHz/outConf  arrays paralelos de `maxPoints`. Cualquiera nulo o
 *                      `maxPoints <= 0` ⇒ 0.
 */
inline int pitchSeriesOverMono(const float* mono, int regionFrames, int sampleRate,
                               int regionStart, int hopFrames, int windowFrames,
                               int* outFrames, float* outHz, float* outConf,
                               int maxPoints) {
    if (mono == nullptr || outFrames == nullptr || outHz == nullptr || outConf == nullptr) return 0;
    if (maxPoints <= 0 || sampleRate <= 0 || hopFrames <= 0) return 0;
    const int total = pitchPointCount(regionFrames, windowFrames, hopFrames);
    if (total <= 0) return 0;

    wma::dsp::McLeodWindowedPitch detector;
    detector.prepareWindowed(sampleRate, windowFrames, kVoiceMinHz, kVoiceMaxHz);
    detector.loadMonoRegion(mono, regionFrames);
    wma::analysis::SpectralSupportProbe probe(windowFrames);

    int written = 0;
    for (int start = 0; start + windowFrames <= regionFrames && written < maxPoints;
         start += hopFrames) {
        float hz = 0.0f;
        float conf = 0.0f;
        if (detector.estimateWindowAt(start) && detector.hasEstimate()) {
            // La ventana ENTERA reemplaza el ring del probe: el soporte se juzga sobre
            // exactamente los frames que produjeron la altura, no sobre un ring que viene
            // de antes.
            probe.pushMono(mono + start, windowFrames);
            const double support = probe.supportDb(sampleRate, detector.estimatedHz());
            if (support >= kSupportFloorDb) {
                hz = static_cast<float>(detector.estimatedHz());
                conf = static_cast<float>(detector.estimatedClarity());
            }
        }
        outFrames[written] = regionStart + start + windowFrames / 2;
        outHz[written] = hz;
        outConf[written] = conf;
        ++written;
    }
    return written;
}

/// Cuantos bins tiene la envolvente: `floor(regionFrames / hopFrames)`.
inline int envelopeBinCount(int regionFrames, int hopFrames) {
    if (regionFrames <= 0 || hopFrames <= 0) return 0;
    return regionFrames / hopFrames;
}

/**
 * @brief La envolvente RMS cruda, lineal [0, 1], ventana = hop, cola descartada.
 *        `outBins[k]` cubre `[regionStart + k·hop, regionStart + (k+1)·hop)`.
 *        Escribe hasta `maxBins`; devuelve cuantos escribio.
 */
inline int levelEnvelopeOverMono(const float* mono, int regionFrames, int hopFrames,
                                 float* outBins, int maxBins) {
    if (mono == nullptr || outBins == nullptr || maxBins <= 0) return 0;
    const int total = envelopeBinCount(regionFrames, hopFrames);
    if (total <= 0) return 0;
    const int bins = total < maxBins ? total : maxBins;
    for (int k = 0; k < bins; ++k) {
        const float* w = mono + static_cast<std::ptrdiff_t>(k) * hopFrames;
        double energy = 0.0;
        for (int i = 0; i < hopFrames; ++i) {
            const double v = w[i];
            energy += v * v;
        }
        outBins[k] = static_cast<float>(std::sqrt(energy / hopFrames));
    }
    return bins;
}

}  // namespace wma::track_analysis
