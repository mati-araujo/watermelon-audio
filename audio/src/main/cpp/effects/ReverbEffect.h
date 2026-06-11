#ifndef REVERB_EFFECT_H
#define REVERB_EFFECT_H

#include "Effect.h"
#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>

// Include DSP utilities
#include "../dsp/DelayLine.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/StereoTools.h"
#include "../dsp/EarlyReflections.h"
#include "../dsp/LFO.h"

/**
 * @file ReverbEffect.h
 * @brief Reverberación profesional inspirada en Vox y Pink Floyd
 *
 * Versión mejorada con:
 * - Pre-delay (0-100ms)
 * - Early reflections con control de difusión
 * - Late reverb (Freeverb mejorado) con modulación
 * - Tone controls (HPF/LPF)
 * - True stereo processing
 * - Damping mejorado
 * - 12 parámetros totales (compatible con versión anterior)
 *
 * Arquitectura:
 *   Input → Pre-Delay → [Early Reflections + Late Reverb (Modulated)] → Tone Control → Stereo → Output
 *                              ↓                ↓          ↓
 *                          (Param 7)      (Params 8, 9: Modulation)
 *
 * Iteración 1 (MVP): Pre-delay, Tone, True Stereo, Damping
 * Iteración 2: Early reflections + Diffusion control
 * Iteración 3 (ACTUAL): Modulación con LFO para shimmer effect
 */

/**
 * @class ReverbEffect
 * @brief Efecto de reverberación profesional
 *
 * Combina algoritmo Freeverb mejorado con componentes profesionales:
 * - PreDelayLine para separación inicial
 * - Biquad filters para tone shaping
 * - StereoDecorrelator para imagen estéreo amplia
 */
class ReverbEffect : public Effect {
public:
    /**
     * @brief Constructor por defecto.
     *
     * Inicializa los buffers de delay para los filtros comb y allpass,
     * y calcula los parámetros iniciales.
     */
    ReverbEffect();

    /**
     * @brief Establece el tiempo de decay de la reverberación.
     * @param d Tiempo de decay en segundos (0.1-5.0).
     */
    void setDecay(float d);

    /**
     * @brief Establece el tamaño de la sala virtual.
     * @param s Tamaño relativo de la sala (0.5-2.0).
     */
    void setSize(float s);

    /**
     * @brief Establece la mezcla wet/dry.
     * @param m Nivel de señal reverberada (0.0-1.0).
     */
    void setMix(float m);

    /**
     * @brief Procesa el audio a través de la reverberación.
     * @param input Buffer de entrada estéreo.
     * @param output Buffer de salida estéreo.
     * @param numFrames Número de frames a procesar.
     */
    void process(float* input, float* output, int numFrames) override;

    /**
     * @brief Establece un parámetro del efecto.
     * @param paramId ID del parámetro (0: decay, 1: size, 2: mix).
     * @param value Valor del parámetro.
     */
    void setParam(int paramId, float value) override;

    /**
     * @brief Obtiene el valor de un parámetro del efecto.
     * @param paramId ID del parámetro (0: decay, 1: size, 2: mix).
     * @return Valor del parámetro.
     */
    float getParam(int paramId) override;

    /**
     * @brief Establece el sample rate del efecto.
     * @param sampleRate Sample rate en Hz (ej: 48000).
     */
    void setSampleRate(int sampleRate) override;

    /**
     * @brief Clear all DSP state (reverb tail) without resizing buffers.
     *
     * Zero-fills comb and allpass delay buffers, resets their read/write
     * positions and filter memory, and clears sub-component state. RT-safe:
     * no allocation, no resizing. Use from the audio thread when the
     * effect has been processing loud audio and a clean slate is needed
     * — e.g. the chaos_pad → input_fx transition, where a seconds-long
     * reverb tail cooked by synth audio would otherwise bleed into the
     * first blocks of mic processing.
     */
    void reset() override;

    // Nuevos setters para parámetros expandidos
    void setPreDelay(float delayMs);        // Param 3: 0-100ms
    void setDamping(float amount);          // Param 4: 0-1
    void setDiffusion(float amount);        // Param 5: 0-1 (Iteración 2)
    void setStereoWidth(float width);       // Param 6: 0-1
    void setEarlyLateMix(float mix);        // Param 7: 0-1 (Iteración 2)
    void setModDepth(float depth);          // Param 8: 0-1 (Iteración 3)
    void setModRate(float rateHz);          // Param 9: 0.1-5Hz (Iteración 3)
    void setLowCut(float frequency);        // Param 10: 20-500Hz
    void setHighCut(float frequency);       // Param 11: 1k-20kHz

private:
    // ============================================================================
    // SAMPLE RATE
    // ============================================================================
    int mSampleRate = 48000;

    // ============================================================================
    // PARÁMETROS (12 total, atomics para thread-safety)
    // ============================================================================
    // Parámetros básicos (compatibles con versión anterior)
    std::atomic<float> decay{2.0f};         // Param 0: 0.1-10.0s
    std::atomic<float> size{0.8f};          // Param 1: 0.1-2.0
    std::atomic<float> mix{0.3f};           // Param 2: 0-1

    // Parámetros nuevos (Iteración 1)
    std::atomic<float> preDelayMs{20.0f};   // Param 3: 0-100ms
    std::atomic<float> damping{0.5f};       // Param 4: 0-1
    std::atomic<float> stereoWidth{1.0f};   // Param 6: 0-1
    std::atomic<float> lowCutFreq{80.0f};   // Param 10: 20-500Hz
    std::atomic<float> highCutFreq{12000.0f}; // Param 11: 1k-20kHz

    // Parámetros reservados para futuras iteraciones
    std::atomic<float> diffusion{0.7f};        // Param 5: 0-1 (Iteración 2)
    std::atomic<float> earlyLateMix{0.3f};     // Param 7: 0-1 (Iteración 2)
    std::atomic<float> modDepth{0.0f};         // Param 8: 0-1 (Iteración 3)
    std::atomic<float> modRate{0.5f};          // Param 9: 0.1-5Hz (Iteración 3)

    // ============================================================================
    // COMPONENTES DSP
    // ============================================================================
    // Iteración 1
    DelayLine preDelayLine;                  // Pre-delay (0-100ms)
    BiquadFilter lowCutFilter;               // HPF para low cut
    BiquadFilter highCutFilter;              // LPF para high cut
    StereoDecorrelator stereoProcessor;      // True stereo desde mono reverb

    // Iteración 2
    EarlyReflections earlyReflections;       // Early reflections con diffusion

    // Iteración 3
    LFO modulationLFOs[4];                   // 4 LFOs para modular comb filters

    // ============================================================================
    // LATE REVERB (Freeverb - componentes originales)
    // ============================================================================
    std::vector<float> combBuffers[4];
    std::atomic<int> combPos[4];
    std::atomic<float> combGains[4];
    float combPrevFiltered[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    std::vector<float> allpassBuffers[2];
    std::atomic<int> allpassPos[2];
    float allpassGain = 0.7f;

    // ============================================================================
    // HELPERS
    // ============================================================================
    /**
     * @brief Actualiza parámetros de late reverb (comb gains)
     */
    void updateParameters();

    /**
     * @brief Procesa late reverb (comb + allpass filters)
     * @return Mono reverb tail
     */
    float processLateReverb(float monoInput);
};

#endif // REVERB_EFFECT_H
