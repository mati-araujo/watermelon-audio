#pragma once

// Deteccion de onsets sobre cualquier fuente de muestras (REQ-055 S15).
//
// Es el algoritmo de `TrackBuffer::detectOnsets`, sacado a una funcion pura para que corra
// sobre un buffer que no es una pista del looper (el re-render de una toma de Studio) con el
// MISMO resultado: la calibracion de S2 (hop 256, margen de 10 ms) vale para los dos
// caminos solo si es una unica implementacion. NO ES RT: corre en el hilo de UI/IO, asi que
// sus std::vector con try/catch se quedan.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wma::onset {

/**
 * Onsets (transitorios) por flujo de energia, para derivar el tempo de una toma libre desde
 * su RITMO (intervalos entre onsets) y no desde su largo total.
 *
 * Algoritmo:
 *   1. Energia de corto plazo por ventana (suma de L²+R² sobre `hopFrames`).
 *   1b. Suavizado de esa energia (media movil de ±2 ventanas) ANTES del flujo: una nota
 *      sostenida cuyo periodo supera el hop hace oscilar la energia cruda en cada bloque, y
 *      el flujo lo lee como una racha de onsets falsos.
 *   2. Flujo positivo de log-energia (primera diferencia rectificada) sobre la envolvente
 *      suavizada: el dominio log mantiene comparables los golpes suaves y los fuertes.
 *   3. Pico adaptivo: una ventana es onset si su flujo supera un piso absoluto (~ +3 dB),
 *      supera la media local por un factor, es un maximo local y cae >= ~50 ms despues del
 *      onset previo (rechaza flams y dobles disparos).
 *
 * @param sample      `(frame, channel) -> float`, canal 0 = L y 1 = R; se llama solo con
 *                    `0 <= frame < length`.
 * @param length      Frames de la fuente.
 * @param outOnsets   Frame de cada onset, ascendente y relativo al inicio de la fuente.
 * @param maxOnsets   Capacidad de `outOnsets`: no se escribe nada mas alla.
 * @param hopFrames   Ventana de analisis en frames (256 ≈ 5,3 ms a 48 kHz); menos de 32 sube a 32.
 * @param sensitivity >1 = mas onsets (umbral mas bajo), <1 = menos (~1,0 por defecto).
 * @return onsets escritos (0 si es silencio, muy corto o invalido); nunca negativo.
 */
template <typename SampleFn>
int detect(SampleFn&& sample, int length, int sampleRate, int hopFrames, float sensitivity,
           int* outOnsets, int maxOnsets) {
    if (!outOnsets || maxOnsets <= 0) return 0;
    if (hopFrames < 32) hopFrames = 32;
    if (static_cast<int64_t>(length) <= static_cast<int64_t>(hopFrames) * 4) return 0;

    const int numWin = length / hopFrames;
    if (numWin < 4) return 0;

    std::vector<float> energy, smoothed, flux;
    try {
        energy.assign(static_cast<size_t>(numWin), 0.0f);
        smoothed.assign(static_cast<size_t>(numWin), 0.0f);
        flux.assign(static_cast<size_t>(numWin), 0.0f);
    } catch (...) {
        return 0;
    }

    // 1) Energia de corto plazo por ventana.
    for (int w = 0; w < numWin; ++w) {
        const int base = w * hopFrames;
        float e = 0.0f;
        for (int i = 0; i < hopFrames; ++i) {
            const float l = sample(base + i, 0);
            const float r = sample(base + i, 1);
            e += l * l + r * r;
        }
        energy[w] = e;
    }

    // 1b) Suavizado (ver el encabezado): un pad sostenido llego a reportar >100 onsets en 6 s.
    const int sm = 2;
    for (int w = 0; w < numWin; ++w) {
        const int a = std::max(0, w - sm);
        const int b = std::min(numWin - 1, w + sm);
        float sum = 0.0f;
        for (int k = a; k <= b; ++k) sum += energy[k];
        smoothed[w] = sum / static_cast<float>(b - a + 1);
    }

    // 2) Flujo positivo de log-energia sobre la envolvente suavizada.
    for (int w = 1; w < numWin; ++w) {
        const float d = std::log(smoothed[w] + 1e-9f) - std::log(smoothed[w - 1] + 1e-9f);
        flux[w] = (d > 0.0f) ? d : 0.0f;
    }

    // 3) Pico adaptivo.
    const float mult = (sensitivity > 0.01f) ? (1.0f / sensitivity) : 1.0f;
    // Piso absoluto de flujo (≈ +3 dB). El umbral adaptivo solo sube con el flujo local, asi que
    // el material tonal sostenido (cuya energia oscila en cada bloque) lo cruza todo el tiempo:
    // un pad sostenido reportaba ~88 onsets en 5 s. Un transitorio real salta varios dB (flujo
    // muy por encima); la oscilacion de un tono estable no. Escala con `sensitivity`.
    const float minFlux = 0.5f * mult;
    const int half = 8;  // ventana de la media local (± ventanas)
    const int minGapWin =
        std::max(1, static_cast<int>(0.05f * static_cast<float>(sampleRate)
                                     / static_cast<float>(hopFrames)));
    int count = 0;
    int lastOnsetWin = -minGapWin - 1;
    for (int w = 1; w < numWin - 1 && count < maxOnsets; ++w) {
        const float f = flux[w];
        if (f <= minFlux) continue;
        const int a = std::max(1, w - half);
        const int b = std::min(numWin - 1, w + half);
        float sum = 0.0f;
        int n = 0;
        for (int k = a; k < b; ++k) { sum += flux[k]; ++n; }
        const float localMean = (n > 0) ? sum / static_cast<float>(n) : 0.0f;
        const float threshold = localMean * 1.5f * mult + 1.0e-4f;
        if (f > threshold && f >= flux[w - 1] && f > flux[w + 1]
            && (w - lastOnsetWin) >= minGapWin) {
            outOnsets[count++] = w * hopFrames;
            lastOnsetWin = w;
        }
    }
    return count;
}

}  // namespace wma::onset
