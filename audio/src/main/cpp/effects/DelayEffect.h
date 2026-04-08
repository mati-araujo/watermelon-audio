#ifndef DELAY_EFFECT_H
#define DELAY_EFFECT_H

#include "Effect.h"
#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>

/**
 * @file DelayEffect.h
 * @brief Implementación de un efecto de delay con sincronización BPM opcional.
 */

/**
 * @class DelayEffect
 * @brief Clase que implementa un efecto de delay digital con feedback.
 *
 * Esta clase proporciona un efecto de delay estéreo con control de tiempo de delay,
 * feedback y mezcla wet/dry. Soporta sincronización con BPM para delays musicales.
 */
class DelayEffect : public Effect {
public:
    /**
     * @brief Constructor por defecto.
     *
     * Inicializa los buffers de delay y calcula el tamaño inicial de delay.
     */
    DelayEffect();

    /**
     * @brief Establece el tiempo de delay.
     * @param dt Tiempo de delay en milisegundos (1-2000 ms).
     */
    void setDelayTime(float dt);

    /**
     * @brief Establece el nivel de feedback.
     * @param fb Nivel de feedback (0.0-0.9).
     */
    void setFeedback(float fb);

    /**
     * @brief Establece la mezcla wet/dry.
     * @param w Nivel de señal wet (0.0-1.0).
     */
    void setWet(float w);

    /**
     * @brief Establece el BPM para sincronización.
     * @param b BPM (60-200).
     */
    void setBpm(float b) override;

    /**
     * @brief Establece la división de nota para sincronización.
     * @param nd División de nota (1-32).
     */
    void setNoteDivision(float nd);

    /**
     * @brief Activa o desactiva la sincronización BPM.
     * @param s true para activar sincronización, false para tiempo manual.
     */
    void setSync(bool s);

    /**
     * @brief Procesa el audio a través del delay.
     * @param input Buffer de entrada estéreo.
     * @param output Buffer de salida estéreo.
     * @param numFrames Número de frames a procesar.
     */
    void process(float* input, float* output, int numFrames) override;

    /**
     * @brief Establece un parámetro del efecto.
     * @param paramId ID del parámetro (0: delayTime, 1: feedback, 2: wet, 3: bpm, 4: noteDivision, 5: sync).
     * @param value Valor del parámetro.
     */
    void setParam(int paramId, float value) override;

    /**
     * @brief Obtiene el valor de un parámetro del efecto.
     * @param paramId ID del parámetro (0: delayTime, 1: feedback, 2: wet, 3: bpm, 4: noteDivision, 5: sync).
     * @return Valor del parámetro.
     */
    float getParam(int paramId) override;

    /**
     * @brief Establece el sample rate del efecto.
     * @param sampleRate Sample rate en Hz (ej: 48000).
     */
    void setSampleRate(int sampleRate) override;

private:
    int mSampleRate = 48000;  /**< Current sample rate */
    std::atomic<float> delayTime{500.0f}; /**< Tiempo de delay en ms (atómico) */
    std::atomic<float> feedback{0.5f}; /**< Nivel de feedback (atómico) */
    std::atomic<float> wet{0.3f}; /**< Mezcla wet/dry (atómica) */
    std::atomic<float> bpm{120.0f}; /**< BPM para sincronización (atómico) */
    std::atomic<float> noteDivision{16.0f}; /**< División de nota (atómica) */
    bool sync = true; /**< Flag de sincronización BPM */

    // FIXED: Hacer delaySamples atómico para evitar race conditions
    std::atomic<int> delaySamples{0}; /**< Número de muestras de delay calculadas (atómico) */
    std::vector<float> bufferL, bufferR; /**< Buffers circulares para delay estéreo */
    std::atomic<int> writePos{0}; /**< Posición de escritura en el buffer (atómico para thread-safety) */

    /**
     * @brief Actualiza el número de muestras de delay basado en los parámetros actuales.
     *
     * Si la sincronización está activada, calcula el delay basado en BPM y división de nota.
     * De lo contrario, usa el tiempo de delay manual.
     * FIXED: Usa atomic store para thread-safety.
     */
    void updateDelaySamples();
};

#endif // DELAY_EFFECT_H