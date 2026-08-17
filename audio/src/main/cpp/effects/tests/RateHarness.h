#ifndef WMA_RATE_HARNESS_H
#define WMA_RATE_HARNESS_H

/**
 * WD-2.3 — el instrumental para medir el MISMO efecto a VARIOS sample rates.
 *
 * QUE PROBLEMA RESUELVE
 * ---------------------
 * `GoldenHarness.h` mide un efecto a 48 kHz y lo compara contra un archivo.
 * Esto mide el MISMO efecto a 44,1 / 48 / 96 kHz y compara las tres medidas
 * ENTRE SI. No hay golden: el patron de referencia es el propio efecto a otro
 * rate, y lo que se afirma es que la magnitud fisica —hertz, milisegundos,
 * decibeles— no depende de cuantas muestras por segundo entrega el device.
 *
 * POR QUE NO SE COMPARA LA CURVA |H(f)| ENTERA
 * --------------------------------------------
 * Porque seria afirmar algo FALSO. Un biquad diseñado por transformada bilineal
 * comprime el eje de frecuencia cerca de Nyquist: la banda de paso y el codo
 * caen donde se pidieron, pero la banda de rechazo NO tiene la misma forma a
 * dos rates distintos. Medido sobre `FilterEffect` con un LPF a 1 kHz:
 *
 *     banda           <=500 Hz   <=2 kHz   <=5 kHz   toda la reticula
 *     desviacion         0,001     0,031     0,353         9,284  dB
 *
 * Nueve decibeles a 15,9 kHz no son un defecto: son el warping del metodo de
 * diseño, y desaparecerian recien con un prewarping por banda que esta libreria
 * no hace (ni tiene por que). Un test que exigiera curvas iguales estaria
 * exigiendo que el filtro deje de ser un biquad bilineal.
 *
 * Lo que SI es invariante es el LANDMARK: la frecuencia donde el filtro cae
 * 3 dB. Medida a los tres rates para un LPF a 2 kHz: 1997,36 / 1997,36 /
 * 1997,34 Hz. Eso es lo que promete la perilla, y es lo que se afirma.
 *
 * LAS TRES MAGNITUDES QUE SE MIDEN
 * --------------------------------
 *   1. HERTZ         — el landmark de -3 dB de un filtro.
 *   2. MILISEGUNDOS  — donde vuelve el eco de un delay.
 *   3. HERTZ (lento) — el ciclo de un LFO de modulacion.
 *
 * Y una cuarta, mas debil y por eso aplicable a los 23: el NIVEL de salida
 * (RMS) ante una misma señal musical. No dice que el efecto suene igual, dice
 * que suena igual de fuerte — y eso alcanza para cachar un efecto cuyo estado
 * interno quedo preparado para otro rate, que es la clase de defecto de WD-3.4.
 */

#include "GoldenHarness.h"

#include "../Effect.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wma::rate {

using wma::golden::captureImpulseResponse;
using wma::golden::kPi;
using wma::golden::responseDbAt;
using wma::golden::toDb;

/// Los tres rates del requerimiento. 44,1 es el nativo de una fraccion grande
/// de dispositivos Bluetooth y USB; 96 es el que piden las interfaces de audio.
inline constexpr int kRates[3] = {44100, 48000, 96000};

inline const char* rateName(int rate) {
    switch (rate) {
        case 44100: return "44,1 kHz";
        case 48000: return "48 kHz";
        case 96000: return "96 kHz";
        default: return "(otro)";
    }
}

// ---------------------------------------------------------------------------
// Señales — parametrizadas en SEGUNDOS y HERTZ, nunca en samples
// ---------------------------------------------------------------------------

/// Cuantos frames son `seconds` a `rate`. La ventana de captura tiene que
/// medirse en tiempo: 8192 frames son 170 ms a 48 kHz y 85 ms a 96 kHz, y
/// comparar dos ventanas de distinta duracion mide la ventana, no el efecto.
inline int framesFor(double seconds, int rate) {
    return static_cast<int>(seconds * rate);
}

inline std::vector<float> sineStereo(double hz, double seconds, int rate, float amp) {
    const int frames = framesFor(seconds, rate);
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = amp * static_cast<float>(std::sin(2.0 * kPi * hz * i / rate));
        b[static_cast<size_t>(i) * 2] = v;
        b[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return b;
}

/// Continua a nivel constante: para medir la envolvente que impone un LFO de
/// amplitud sin que el propio audio aporte su ciclo a la medicion.
inline std::vector<float> dcStereo(double seconds, int rate, float amp) {
    return std::vector<float>(static_cast<size_t>(framesFor(seconds, rate)) * 2, amp);
}

// ---------------------------------------------------------------------------
// Ejecucion
// ---------------------------------------------------------------------------

/// Corre la señal entera por el efecto en bloques y devuelve la salida.
/// El bloque va en FRAMES fijos a proposito: es lo que hace un backend real, y
/// el troceado forma parte de lo que se mide.
inline std::vector<float> runBlocks(Effect& fx, std::vector<float> in, int blockFrames = 512) {
    const int frames = static_cast<int>(in.size() / 2);
    std::vector<float> out(in.size(), 0.0f);
    for (int s = 0; s < frames; s += blockFrames) {
        const int n = std::min(blockFrames, frames - s);
        fx.process(in.data() + static_cast<size_t>(s) * 2,
                   out.data() + static_cast<size_t>(s) * 2, n);
    }
    return out;
}

/// RMS del canal izquierdo, descartando el primer `skipSeconds` para que los
/// smoothers de parametro hayan asentado. Devuelve -1 si aparecio un no-finito.
inline double rmsLeft(const std::vector<float>& buf, int rate, double skipSeconds) {
    const size_t skip = static_cast<size_t>(framesFor(skipSeconds, rate));
    const size_t frames = buf.size() / 2;
    if (skip >= frames) return -1.0;
    double acc = 0.0;
    for (size_t i = skip; i < frames; ++i) {
        const double v = buf[i * 2];
        if (!std::isfinite(v)) return -1.0;
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(frames - skip));
}

// ---------------------------------------------------------------------------
// Landmarks
// ---------------------------------------------------------------------------

/// El pico de |H(f)| sobre la reticula log, en dB. Referencia para el -3.
inline double peakResponseDb(const std::vector<float>& ir, int rate) {
    double best = -1e9;
    for (int i = 0; i <= 400; ++i) {
        const double f = 20.0 * std::pow(1000.0, i / 400.0);
        if (f > rate * 0.45) break;
        best = std::max(best, responseDbAt(ir, f, rate));
    }
    return best;
}

/**
 * La frecuencia donde la respuesta cae 3 dB por debajo de su pico, buscada por
 * biseccion sobre el eje LOGARITMICO — que es el eje en el que un filtro es
 * simetrico, y el unico en el que 60 iteraciones alcanzan para 6 cifras.
 *
 * @param above true para un HPF (el corte esta POR DEBAJO de la banda de paso).
 */
inline double cornerFrequency(const std::vector<float>& ir, int rate, bool above) {
    const double peak = peakResponseDb(ir, rate);
    double lo = 20.0;
    double hi = std::min(20000.0, rate * 0.45);
    for (int i = 0; i < 60; ++i) {
        const double mid = std::sqrt(lo * hi);
        const bool inBand = (responseDbAt(ir, mid, rate) - peak) > -3.0;
        if (above) {
            if (inBand) hi = mid; else lo = mid;
        } else {
            if (inBand) lo = mid; else hi = mid;
        }
    }
    return std::sqrt(lo * hi);
}

/// Indice del pico de mayor magnitud a partir de `fromFrame`. Con el impulso en
/// el frame 0, saltear el directo deja el PRIMER ECO.
inline int peakFrameAfter(const std::vector<float>& ir, int fromFrame) {
    int best = -1;
    float bestV = 0.0f;
    for (int i = fromFrame; i < static_cast<int>(ir.size()); ++i) {
        const float a = std::abs(ir[static_cast<size_t>(i)]);
        if (a > bestV) { bestV = a; best = i; }
    }
    return best;
}

/**
 * Periodo del ciclo de modulacion, en SEGUNDOS, medido por cruces de la media
 * de la envolvente.
 *
 * Por que cruces y no una FFT: el ciclo dura cientos de milisegundos, asi que
 * una FFT necesitaria una ventana de varios segundos para resolverlo y ademas
 * habria que separar el bin del LFO del contenido de la señal. Contar cruces de
 * la media sobre la envolvente es exacto para cualquier forma de onda periodica
 * y no depende de que el LFO sea senoidal.
 *
 * @return periodo en segundos, o -1 si no completo al menos un ciclo.
 */
inline double modulationPeriodSeconds(const std::vector<float>& out, int rate,
                                      double totalSeconds) {
    const int win = rate / 200;  // 5 ms
    const int frames = static_cast<int>(out.size() / 2);
    std::vector<double> env;
    for (int s = 0; s + win < frames; s += win) {
        double acc = 0.0;
        for (int k = 0; k < win; ++k) acc += std::abs(out[static_cast<size_t>(s + k) * 2]);
        env.push_back(acc / win);
    }
    if (env.size() < 4) return -1.0;

    double mean = 0.0;
    for (double v : env) mean += v;
    mean /= static_cast<double>(env.size());

    int crossings = 0;
    for (size_t i = 1; i < env.size(); ++i) {
        if ((env[i - 1] < mean) != (env[i] < mean)) ++crossings;
    }
    if (crossings < 2) return -1.0;
    // Dos cruces por ciclo.
    return 2.0 * totalSeconds / crossings;
}

/// Desviacion relativa maxima entre tres medidas, como fraccion del promedio.
inline double relativeSpread(const double v[3]) {
    const double mean = (v[0] + v[1] + v[2]) / 3.0;
    if (mean == 0.0) return 0.0;
    double worst = 0.0;
    for (int i = 0; i < 3; ++i) worst = std::max(worst, std::abs(v[i] - mean));
    return worst / std::abs(mean);
}

/// Desviacion maxima entre tres niveles, en dB.
inline double spreadDb(const double v[3]) {
    double worst = 0.0;
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            if (v[a] > 0.0 && v[b] > 0.0) {
                worst = std::max(worst, std::abs(20.0 * std::log10(v[a] / v[b])));
            }
        }
    }
    return worst;
}

}  // namespace wma::rate

#endif  // WMA_RATE_HARNESS_H
