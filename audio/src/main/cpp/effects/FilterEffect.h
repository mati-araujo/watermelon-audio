#ifndef FILTER_EFFECT_H
#define FILTER_EFFECT_H

#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <cmath>
#include <algorithm>

/**
 * @file FilterEffect.h
 * @brief Implementación de un filtro de audio digital con tipos LPF, HPF y BPF.
 */

/**
 * @class FilterEffect
 * @brief Clase que implementa un filtro de audio digital de segundo orden.
 *
 * Esta clase proporciona filtrado de audio en tiempo real con tres tipos de filtro:
 * paso bajo (LPF), paso alto (HPF) y banda de paso (BPF). Utiliza un diseño de filtro
 * IIR con coeficientes calculados en tiempo real para cambios de frecuencia y resonancia.
 *
 * PHASE 4: Rango Q expandido (0.5-30.0) para efectos profesionales y self-oscillation.
 */
class FilterEffect : public Effect {
public:
    // PHASE 4: Constantes de rango Q profesional
    static constexpr float MIN_Q = 0.5f;    // Mínimo profesional (antes 0.1)
    static constexpr float MAX_Q = 30.0f;   // Máximo para self-oscillation (antes 10.0)
    static constexpr float SELF_OSC_THRESHOLD = 15.0f;  // Q donde empieza auto-oscilación

    /**
     * @enum FilterType
     * @brief Tipos de filtro disponibles.
     *
     * El underlying type es fijo a propósito. `setParam(2, x)` castea un valor
     * arbitrario del host a este enum, y para un enum *sin* underlying type
     * fijo el rango válido es sólo el que necesitan sus enumeradores (acá
     * 0..3): guardar 9 y volver a leerlo es undefined behaviour, y UBSan lo
     * marca con razón. Fijarlo a `int` hace representable cualquier int, así
     * el fallback de tipo desconocido en updateCoefficients() queda bien
     * definido en vez de simplemente andar por suerte.
     */
    enum FilterType : int {
        LPF, /**< Filtro paso bajo */
        HPF, /**< Filtro paso alto */
        BPF  /**< Filtro banda de paso */
    };

    /**
     * @brief Constructor por defecto.
     *
     * Inicializa el filtro con valores predeterminados y calcula los coeficientes iniciales.
     */
    FilterEffect();

    /**
     * @brief Establece la frecuencia de corte del filtro.
     * @param freq Frecuencia de corte en Hz (20-20000).
     */
    void setCutoff(float freq);

    /**
     * @brief Establece la resonancia del filtro.
     * @param res Valor de resonancia (0.1-10.0).
     */
    void setResonance(float res);

    /**
     * @brief Establece el tipo de filtro.
     * @param t Tipo de filtro (LPF, HPF o BPF).
     */
    void setType(FilterType t);

    /**
     * @brief Procesa el audio a través del filtro.
     * @param input Buffer de entrada estéreo.
     * @param output Buffer de salida estéreo.
     * @param numFrames Número de frames a procesar.
     */
    void process(float* input, float* output, int numFrames) override;

    /**
     * @brief Establece un parámetro del efecto.
     * @param paramId ID del parámetro (0: cutoff, 1: resonance, 2: type).
     * @param value Valor del parámetro.
     */
    void setParam(int paramId, float value) override;

    /**
     * @brief Obtiene el valor de un parámetro del efecto.
     * @param paramId ID del parámetro (0: cutoff, 1: resonance, 2: type).
     * @return Valor del parámetro.
     */
    float getParam(int paramId) override;

    /**
     * @brief Establece el sample rate del efecto.
     * @param sampleRate Sample rate en Hz (ej: 48000).
     */
    void setSampleRate(int sampleRate) override;

    /**
     * @brief Limpia la memoria del biquad y re-siembra los smoothers (WD-3.2).
     *
     * No toca cutoff, resonance ni type: eso es configuracion del usuario, no
     * estado. Tampoco los coeficientes, que se derivan de esos parametros.
     */
    void reset() override;

private:
    int mSampleRate = 48000;  /**< Current sample rate */
    std::atomic<float> cutoff{1000.0f}; /**< Frecuencia de corte atómica para thread-safety */
    std::atomic<float> resonance{0.707f}; /**< Resonancia atómica para thread-safety */
    FilterType type{LPF}; /**< Tipo de filtro actual */

    // FIXED: Estructura de coeficientes para atomic swap (evita race conditions)
    struct Coefficients {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    };

    // Doble buffer de coeficientes - el RT thread lee, el UI thread escribe
    Coefficients coeffs[2];
    std::atomic<int> currentCoeffsIndex{0};  // Índice atómico para swap lock-free

    // Estados del filtro para canal izquierdo
    float x1_l = 0.0f, x2_l = 0.0f, y1_l = 0.0f, y2_l = 0.0f;

    // Estados del filtro para canal derecho
    float x1_r = 0.0f, x2_r = 0.0f, y1_r = 0.0f, y2_r = 0.0f;

    // IMPROVED: Parameter smoothers to prevent clicks on parameter changes
    ParameterSmoother cutoffSmoother{0.995f};     // ~10ms smoothing time at 48kHz
    ParameterSmoother resonanceSmoother{0.995f};  // ~10ms smoothing time at 48kHz

    // PHASE 4: Gain compensation for high Q values (self-oscillation control)
    float mGainCompensation{1.0f};

    /**
     * @brief Actualiza los coeficientes del filtro basados en los parámetros actuales.
     *
     * Calcula los coeficientes b0, b1, b2, a1, a2 usando la transformación bilinear
     * para un filtro analógico de segundo orden.
     * FIXED: Usa doble buffer para evitar race conditions.
     */
    void updateCoefficients();
};

#endif // FILTER_EFFECT_H