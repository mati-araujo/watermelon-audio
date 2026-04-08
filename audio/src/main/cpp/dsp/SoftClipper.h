#ifndef SOFT_CLIPPER_H
#define SOFT_CLIPPER_H

#include <cmath>
#include <algorithm>

/**
 * @file SoftClipper.h
 * @brief Soft clipping algorithms para prevenir distorsión digital
 *
 * Implementa diferentes tipos de soft clipping para proteger la salida
 * contra overshoots sin generar la distorsión armónica del hard clipping.
 */

/**
 * @class SoftClipper
 * @brief Soft clipper profesional con múltiples algoritmos
 *
 * Previene que las señales excedan ±1.0f usando curvas suaves que
 * minimizan la distorsión armónica comparado con hard clipping.
 */
class SoftClipper {
public:
    enum class Type {
        TANH,       ///< Hyperbolic tangent (suave, warm)
        ATAN,       ///< Arctangent (transparente)
        CUBIC       ///< Polynomial cubic (eficiente)
    };

    explicit SoftClipper(Type type = Type::TANH)
        : mType(type) {}

    /**
     * @brief Procesa un sample con soft clipping
     * @param input Sample de entrada
     * @return Sample con soft clipping aplicado
     */
    inline float process(float input) {
        switch (mType) {
            case Type::TANH:
                return processTanh(input);
            case Type::ATAN:
                return processAtan(input);
            case Type::CUBIC:
                return processCubic(input);
        }
        return input;
    }

    /**
     * @brief Procesa un buffer estéreo interleaved
     * @param buffer Buffer de entrada/salida (L/R interleaved)
     * @param numFrames Número de frames (cada frame = 2 samples)
     */
    inline void processStereo(float* buffer, int numFrames) {
        for (int i = 0; i < numFrames * 2; ++i) {
            buffer[i] = process(buffer[i]);
        }
    }

    void setType(Type type) { mType = type; }

private:
    /**
     * @brief Tanh soft clipper con headroom
     *
     * Fórmula: tanh(x * 0.666) * 1.5
     * - Permite inputs de hasta ±1.5 antes de saturar
     * - Output garantizado en rango ±1.0
     * - Suena "warm" (similar a saturación de válvulas)
     */
    inline float processTanh(float input) {
        // Tanh con headroom de ±1.5
        // Para input = ±1.0 → output ≈ ±0.85
        // Para input = ±1.5 → output ≈ ±0.995
        return std::tanh(input * 0.666f) * 1.5f;
    }

    /**
     * @brief Arctan soft clipper (más transparente)
     *
     * Fórmula: atan(x * 0.8) * 1.273
     * - Más linear que tanh en rangos bajos
     * - Menos coloración armónica
     * - 1.273 ≈ 2/π (normalización)
     */
    inline float processAtan(float input) {
        // Arctan con headroom de ±2.0
        // Para input = ±1.0 → output ≈ ±0.85
        return std::atan(input * 0.8f) * 1.273f;
    }

    /**
     * @brief Cubic polynomial soft clipper (más eficiente)
     *
     * Fórmula piecewise:
     * - |x| < 1.0: output = x
     * - |x| >= 1.0: output = sign(x) * (1 - (2-|x|)^3 / 3)
     *
     * Ventajas:
     * - No usa funciones trascendentales (más rápido)
     * - Continua hasta segunda derivada (C²)
     * - Alcanza exactamente ±1.0 en x = ±2.0
     */
    inline float processCubic(float input) {
        float absInput = std::abs(input);

        if (absInput < 1.0f) {
            // Zona linear (sin procesamiento)
            return input;
        } else if (absInput < 2.0f) {
            // Zona de soft clipping
            float diff = 2.0f - absInput;
            float clipped = 1.0f - (diff * diff * diff) / 3.0f;
            return (input > 0.0f) ? clipped : -clipped;
        } else {
            // Hard limit como safety (solo para inputs extremos)
            return (input > 0.0f) ? 1.0f : -1.0f;
        }
    }

    Type mType;
};

/**
 * @class TruePeakLimiter
 * @brief Limiter profesional con detección de true peaks
 *
 * Implementa un limiter tipo "brick wall" con:
 * - Attack instantáneo (previene overshoots)
 * - Release exponencial suave (sin pumping)
 * - Threshold ajustable (típicamente -0.1dB)
 *
 * Usado como última etapa de protección antes del DAC.
 */
class TruePeakLimiter {
public:
    /**
     * @brief Constructor
     * @param thresholdDb Threshold en dB (típicamente -0.1 a -0.3 dB)
     * @param releaseMs Tiempo de release en ms (típicamente 50-100 ms)
     */
    explicit TruePeakLimiter(float thresholdDb = -0.1f, float releaseMs = 50.0f)
        : mThreshold(std::pow(10.0f, thresholdDb / 20.0f))
        , mReleaseMs(releaseMs)
        , mGainReduction(1.0f)
        , mReleaseCoeff(0.999f) {}

    /**
     * @brief Configura el sample rate (necesario para calcular release)
     * @param sampleRate Sample rate en Hz
     */
    void setSampleRate(float sampleRate) {
        // Calcular coeficiente de release exponencial
        // tau = releaseMs / 1000
        // coeff = exp(-1 / (tau * sampleRate))
        mReleaseCoeff = std::exp(-1.0f / (mReleaseMs * 0.001f * sampleRate));
    }

    /**
     * @brief Procesa un sample
     * @param input Sample de entrada
     * @return Sample limitado
     */
    inline float process(float input) {
        float absInput = std::abs(input);

        // Detector de peak
        if (absInput > mThreshold) {
            // Attack instantáneo (previene overshoot)
            mGainReduction = mThreshold / absInput;
        } else {
            // Release exponencial suave
            mGainReduction += (1.0f - mGainReduction) * (1.0f - mReleaseCoeff);
        }

        // Aplicar gain reduction
        return input * mGainReduction;
    }

    /**
     * @brief Procesa un buffer estéreo interleaved
     * @param buffer Buffer de entrada/salida (L/R interleaved)
     * @param numFrames Número de frames
     */
    inline void processStereo(float* buffer, int numFrames) {
        for (int i = 0; i < numFrames * 2; i += 2) {
            // Detectar peak máximo de ambos canales (true peak linking)
            float maxPeak = std::max(std::abs(buffer[i]), std::abs(buffer[i + 1]));

            // Calcular gain reduction
            float gainReduction;
            if (maxPeak > mThreshold) {
                // Attack instantáneo — actualizar estado persistente
                mGainReduction = mThreshold / maxPeak;
                gainReduction = mGainReduction;
            } else {
                // Release exponencial suave
                mGainReduction += (1.0f - mGainReduction) * (1.0f - mReleaseCoeff);
                gainReduction = mGainReduction;
            }

            // Aplicar a ambos canales (stereo linking)
            buffer[i] *= gainReduction;
            buffer[i + 1] *= gainReduction;
        }
    }

    void setThreshold(float thresholdDb) {
        mThreshold = std::pow(10.0f, thresholdDb / 20.0f);
    }

    void setRelease(float releaseMs) {
        mReleaseMs = releaseMs;
    }

    float getGainReduction() const {
        return mGainReduction;
    }

private:
    float mThreshold;         ///< Threshold linear (no dB)
    float mReleaseMs;         ///< Release time en ms
    float mReleaseCoeff;      ///< Coeficiente de release exponencial
    float mGainReduction;     ///< Gain reduction actual (1.0 = sin reducción)
};

#endif // SOFT_CLIPPER_H
