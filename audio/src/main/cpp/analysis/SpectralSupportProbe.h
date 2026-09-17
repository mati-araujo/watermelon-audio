#pragma once

/**
 * @file SpectralSupportProbe.h
 * @brief ¿La altura detectada está en la señal? (REQ-031 S1). Vivía dentro de
 *        `AnalysisThread.h`; REQ-043 S1 la separó para que el looper la use sin arrastrar
 *        el lazo entero del afinador. El afinador la incluye desde donde siempre.
 */

#include "AnalysisRing.h"

#include <vector>

namespace wma::analysis {

/**
 * @brief La sonda de SOPORTE ESPECTRAL (REQ-031 S1): ¿la altura detectada está en la señal?
 *
 * QUÉ PREGUNTA CONTESTA, Y POR QUÉ NO LA CONTESTA NADIE MÁS
 * ---------------------------------------------------------
 * El detector grueso encuentra un PERÍODO; el strobe integra fase contra el OBJETIVO. Ninguno de
 * los dos pregunta si la altura publicada tiene energía en la señal — y no pueden: el NSDF de un
 * submúltiplo es alto **por construcción** (un múltiplo del período también es un período), y el
 * strobe sigue los parciales del objetivo, que sin candidatos lo pone el consumidor y no coincide
 * con `detectedHz`. Por eso es una sonda propia, sobre la señal cruda.
 *
 * CÓMO MIDE
 * ---------
 * Goertzel en los 8 primeros armónicos de `detectedHz` sobre los últimos `kWindowFrames` frames
 * mono, con ventana de Hann. La respuesta es `max(X(f), X(2f))` relativo al más fuerte de los
 * ocho, en dB: en una cuerda real —aunque le falte el fundamental— el fundamental o su octava es
 * el pico; en un submúltiplo inventado no hay energía en ninguno de los dos.
 *
 * 🔴 LA VENTANA DE HANN NO ES ADORNO, Y ESTÁ MEDIDO (2026-09-07, tarea 1.1). Los números que
 * fijaron el umbral salían de un Goertzel sobre 0,5 s; el lazo drena 2048 frames (46 ms a 44,1
 * kHz). Sobre esos 2048 frames SIN ventana las poblaciones SE TOCAN: el falso convergido sube
 * hasta −15,2 dB y cruza el umbral de −25, porque la fuga de la ventana rectangular (H3 de la E4
 * está a ~40 bins) se mete en el bin del fundamental inventado. Y acumular a través de ticks
 * —el remedio que la spec anticipaba— NO lo arregla, porque la fuga no es varianza: con 8192
 * frames rectangulares el falso queda en −26,2, pegado al umbral. Con Hann sobre los MISMOS 2048
 * frames el peor aceptado es −1,8 y el mejor rechazado −61,3: 59,5 dB de separación, y el
 * barrido bloque a bloque no oscila (±0,7 dB). El umbral no se movió.
 *
 * El ring es propio y NO el bloque drenado: en vivo `drainOnce()` lee lo que haya en el ring, que
 * son unos milisegundos por vuelta, y un Goertzel sobre 200 frames a 110 Hz no mide nada. Se
 * evalúa cuando el detector produce un veredicto NUEVO, sobre los frames que lo terminaron.
 *
 * Lo toca SOLO el thread de análisis. No es RT y no necesita atómicos.
 */
class SpectralSupportProbe {
public:
    /// La ventana que decidió 1.1: el mismo largo que drena el lazo. En frames de captura,
    /// así que a 48 kHz son 43 ms y a 16 kHz 128 ms; el detector tiene la misma dependencia.
    /// Es el default del afinador; el análisis por pista (REQ-043) pasa la suya.
    static constexpr int kWindowFrames = AnalysisRing::kCapacityFrames / 4;
    /// Cuántos armónicos entran en la referencia. Es lo que se midió; más no separa mejor
    /// (el falso ya queda 60 dB abajo) y menos deja al pico fuera cuando H3 domina.
    static constexpr int kHarmonics = 8;

    /// El afinador: ventana `kWindowFrames`.
    SpectralSupportProbe();

    /**
     * Ventana PROPIA, en frames (REQ-043 S1): el análisis offline por pista evalúa el
     * soporte sobre la MISMA ventana W del detector de voz (40 ms), no sobre los ~43 ms del
     * lazo. La Goertzel y la Hann son las mismas; sólo cambia el largo. Un largo ≤ 0 cae al
     * default. Ya no exige potencia de dos: el ring envuelve por comparación, no por máscara,
     * y para el afinador eso da exactamente los mismos índices.
     */
    explicit SpectralSupportProbe(int windowFrames);

    /// Copia `numFrames` frames mono al ring propio. Bloques de cualquier tamaño. Empujar
    /// exactamente `windowFrames()` frames reemplaza la ventana entera: así la usa REQ-043.
    void pushMono(const float* mono, int numFrames) noexcept;

    /// true cuando el ring ya dio una vuelta entera: antes no hay ventana que evaluar.
    bool isPrimed() const noexcept { return mFilled >= mWindowFrames; }

    int windowFrames() const noexcept { return mWindowFrames; }

    /**
     * `max(X(hz), X(2·hz))` relativo al armónico más fuerte, en dB (≤ 0). Sólo tiene sentido
     * con `isPrimed()`. Devuelve −∞ si no hay energía en ningún armónico: sin energía, no hay
     * soporte.
     */
    double supportDb(int sampleRate, double hz) const noexcept;

private:
    /// Magnitud de Goertzel en `hz` sobre la ventana, ya con Hann.
    double magnitudeAt(int sampleRate, double hz) const noexcept;

    int mWindowFrames{kWindowFrames};
    std::vector<float> mRing;
    std::vector<float> mHann;
    int mWrite{0};
    int mFilled{0};
};

}  // namespace wma::analysis
