#pragma once

/**
 * RenderChecks.h — los dos instrumentos que REQ-055 S2 fija para las etapas que
 * renderizan offline y afirman sobre el resultado (ADR-0008, diseño §S11 y §S15).
 *
 *  - S11 ("0 discontinuidades en las uniones"): `findJunctionDiscontinuities`, un
 *    detector de saltos de muestra en las uniones, RELATIVO a su entorno. Umbral
 *    `kJumpRatio` (+ `kJumpFloor`).
 *  - S15 ("onsets a <= N ms de los grabados"): `onsetsOf` (el `detectOnsets` del
 *    looper sobre un render) y `maxOnsetDifferenceMs`. Umbral `kMaxOnsetDifferenceMs`.
 *
 * Los numeros, su metodo y su margen: docs/dev/render-offline.md. Los calibra y los
 * vigila `test_offline_render_checks.cpp`: si un cambio del motor mueve el piso de
 * los renders limpios por encima del umbral, o deja un corte inyectado sin detectar,
 * ese test se pone rojo antes que S11/S15.
 */

#include "looper/TrackBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wma_test {

/**
 * El detector de S11 es RELATIVO, y no por gusto: un umbral absoluto no separa nada.
 * Medido el 2026-10-26: un render LIMPIO de FM, Granular o Supersaw a volumen de
 * exportacion salta hasta 1,73 de una muestra a la otra (contenido de banda ancha),
 * mientras un corte entre dos tonos graves salta 0,2. Lo que distingue un corte es que
 * salta mucho MAS QUE SU ENTORNO: la discontinuidad es el salto |x[n] - x[n-1]| que
 * supera `kJumpRatio` veces el salto maximo de los `kJumpWindowFrames` frames de cada
 * lado (sin contar n-1..n+1) — la regla de "sin click" de la skill medir-dsp, con el
 * regimen tomado alrededor de cada muestra.
 *
 * Y un piso absoluto `kJumpFloor`: un salto mas chico que -46 dBFS no es audible como
 * click aunque su entorno sea silencio (evita disparar en el ruido de cola).
 *
 * Los numeros: ver `kJumpRatio`.
 */
constexpr int kJumpWindowFrames = 32;   // ~0,7 ms a 48 kHz de cada lado
constexpr float kJumpFloor = 0.005f;

/**
 * 2,0. Calibrado el 2026-10-26 (`RenderChecks.TheJumpThresholdSeparatesCleanRendersFromCuts`,
 * build Debug del host, 48 kHz, bloque 256):
 *  - uniones LIMPIAS (40 frames x 14 renders: 7 engines, secos y con cadena de 4
 *    efectos, master y amplitud a 1): razon maxima 1,28. Margen del umbral: x1,57.
 *  - 80 cortes INYECTADOS (union de dos renders distintos sin fundido, y corte a
 *    silencio, en esos 40 frames): razon p10 0,36, p25 0,78, mediana 2,37. Con 2,0 se
 *    detectan 43 de 80 (54 %); con 1,5, 48 (margen x1,17, demasiado justo); con 4,0, 29.
 *
 * 🔴 LO QUE NO VE: un corte cuyo salto es menor que el doble del movimiento propio de
 * la señal en ese entorno (el 46 % de los inyectados, casi todos entre dos señales de
 * banda ancha o cerca de un cruce por cero). No es un click de una muestra; es un
 * cambio de forma de onda, y este detector no lo afirma. Es NECESARIO para "0
 * discontinuidades en las uniones", no suficiente: S11 hace el fundido igual.
 */
constexpr float kJumpRatio = 2.0f;

/// La razon salto / salto maximo del entorno en el frame `n` (canal `ch`).
inline float jumpRatioAt(const std::vector<float>& stereo, size_t n, int ch) {
    const size_t frames = stereo.size() / 2;
    auto d = [&](size_t i) {
        return std::abs(stereo[2 * i + static_cast<size_t>(ch)] -
                        stereo[2 * i - 2 + static_cast<size_t>(ch)]);
    };
    float around = 0.0f;
    const size_t lo = n > static_cast<size_t>(kJumpWindowFrames) + 1 ? n - kJumpWindowFrames : 1;
    const size_t hi = std::min(frames - 1, n + static_cast<size_t>(kJumpWindowFrames));
    for (size_t i = lo; i <= hi; ++i) {
        if (i + 1 >= n && i <= n + 1) continue;   // n-1, n, n+1 quedan afuera
        around = std::max(around, d(i));
    }
    return d(n) / (around + 1e-9f);
}

/**
 * Las UNIONES con discontinuidad: de los frames `junctions` (donde el llamador sabe que
 * hay una union —el borde de una seccion en S11—), los que tienen un salto mayor que
 * `kJumpFloor` y `kJumpRatio` veces su entorno, en algun canal.
 *
 * Se mira SOLO en las uniones, y no en todo el render, porque medido el 2026-10-26 un
 * render limpio de Supersaw o Granular tiene saltos de 100x-400x su entorno por
 * construccion (el reset de la sierra, el borde de un grano): un detector sobre todo
 * el buffer los reporta como cortes que nadie hizo.
 */
inline std::vector<size_t> findJunctionDiscontinuities(const std::vector<float>& stereo,
                                                       const std::vector<size_t>& junctions,
                                                       float ratio = kJumpRatio,
                                                       float floor = kJumpFloor) {
    std::vector<size_t> out;
    const size_t frames = stereo.size() / 2;
    for (size_t n : junctions) {
        if (n < 2 || n + 1 >= frames) continue;
        for (int ch = 0; ch < 2; ++ch) {
            const float jump = std::abs(stereo[2 * n + static_cast<size_t>(ch)] -
                                        stereo[2 * n - 2 + static_cast<size_t>(ch)]);
            if (jump > floor && jumpRatioAt(stereo, n, ch) > ratio) {
                out.push_back(n);
                break;
            }
        }
    }
    return out;
}

/// La razon salto/entorno en el frame `n`, el mayor de los dos canales (0 bajo el piso).
inline float junctionRatio(const std::vector<float>& stereo, size_t n, float floor = kJumpFloor) {
    float r = 0.0f;
    for (int ch = 0; ch < 2; ++ch) {
        const float jump = std::abs(stereo[2 * n + static_cast<size_t>(ch)] -
                                    stereo[2 * n - 2 + static_cast<size_t>(ch)]);
        if (jump > floor) r = std::max(r, jumpRatioAt(stereo, n, ch));
    }
    return r;
}

/// El salto de muestra absoluto mas grande del render, en cualquier canal.
inline float maxSampleJump(const std::vector<float>& stereo) {
    float m = 0.0f;
    const size_t frames = stereo.size() / 2;
    for (size_t n = 1; n < frames; ++n) {
        m = std::max(m, std::abs(stereo[2 * n] - stereo[2 * n - 2]));
        m = std::max(m, std::abs(stereo[2 * n + 1] - stereo[2 * n - 1]));
    }
    return m;
}

/// El hop con que S15 compara onsets: 256 frames (5,3 ms a 48 kHz), el de la UI.
constexpr int kOnsetHopFrames = 256;

/**
 * Umbral de S15: la diferencia maxima entre los onsets de un re-render y los de la
 * toma. 10 ms (el que proponia ADR-0008 §4; S2 lo confirma con datos).
 *
 * Medido el 2026-10-26 (`RenderChecks.TheOnsetThresholdCoversQuantizationAndDetector`):
 * la toma aplica cada gesto en su frame exacto (bloques de 32), el re-render en el
 * borde de bloque de 256 siguiente, con el gesto corrido 0..224 frames dentro del
 * bloque; KS, FM, Supersaw y SoundFont, 10 notas cada uno, 32 comparaciones: peor
 * diferencia 5,33 ms = UN hop (el detector cuantiza a `kOnsetHopFrames` y el bloque
 * del re-render a 256: los dos errores caen en la misma grilla y no se suman). Margen
 * x1,9.
 *
 * VALE PARA: re-render con bloque <= 256 y hop 256. Con bloque 512 la cuantizacion
 * sola puede llegar a 10,7 ms: S15 fija el bloque. NO cubre el muestreo de gestos a
 * ritmo de UI ni el coalescer (ADR-0008 §4): esa diferencia es de los gestos, no del
 * render, y S15 la mide contra la toma real.
 */
constexpr double kMaxOnsetDifferenceMs = 10.0;

/// Los onsets de un render estereo, por el mismo `detectOnsets` que usa el looper.
inline std::vector<int> onsetsOf(const std::vector<float>& stereo, int sampleRate,
                                 int hopFrames = kOnsetHopFrames, float sensitivity = 1.0f) {
    const int frames = static_cast<int>(stereo.size() / 2);
    TrackBuffer tb;
    if (tb.allocate(frames, sampleRate, 0) == 0) return {};
    for (int n = 0; n < frames; ++n) tb.writeFrame(stereo[2 * n], stereo[2 * n + 1]);
    tb.finalizeRecording();
    std::vector<int> out(256);
    const int count = tb.detectOnsets(out.data(), static_cast<int>(out.size()), hopFrames,
                                      sensitivity);
    out.resize(static_cast<size_t>(std::max(0, count)));
    return out;
}

/**
 * La diferencia maxima, en ms, entre dos listas de onsets emparejadas en orden.
 * Listas de distinto largo: infinito (un onset de mas o de menos no es "cerca").
 */
inline double maxOnsetDifferenceMs(const std::vector<int>& a, const std::vector<int>& b,
                                   int sampleRate) {
    if (a.size() != b.size()) return INFINITY;
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(a[i] - b[i])) * 1000.0 / sampleRate);
    }
    return worst;
}

}  // namespace wma_test
