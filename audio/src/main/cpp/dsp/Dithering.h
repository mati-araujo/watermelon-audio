#ifndef DITHERING_H
#define DITHERING_H

#include <cstdint>
#include <cmath>

/**
 * @file Dithering.h
 * @brief Implementación de dithering para conversión float→int
 *
 * El dithering agrega ruido de baja amplitud para prevenir errores de
 * cuantización correlacionados al convertir de float32 a int16/int24.
 *
 * Tipos implementados:
 * - RPDF (Rectangular PDF): Ruido blanco simple
 * - TPDF (Triangular PDF): Ruido decorrelacionado (recomendado)
 */

/**
 * @class TPDFDitherer
 * @brief Ditherer TPDF (Triangular Probability Density Function)
 *
 * TPDF es el estándar de la industria para dithering porque:
 * - Decorrelaciona completamente el error de cuantización
 * - No requiere noise shaping
 * - Transparente auditivamente en señales de bajo nivel
 * - Permite resolución efectiva por debajo de 1 LSB
 *
 * Implementación:
 * - Suma de dos muestras RPDF independientes = TPDF
 * - Amplitud: ±1 LSB de 16-bit (±1/32768)
 * - RNG: Xorshift32 (rápido, calidad suficiente)
 */
class TPDFDitherer {
public:
    /**
     * @brief Constructor
     * @param targetBits Bits de cuantización objetivo (default: 16)
     *
     * Para int16: targetBits = 16 → amplitud = ±1/32768
     * Para int24: targetBits = 24 → amplitud = ±1/8388608
     */
    explicit TPDFDitherer(int targetBits = 16)
        : mRngState(12345678)  // Seed inicial (diferente por canal)
    {
        // Calcular amplitud de dither según bits objetivo
        // 1 LSB = 1 / (2^targetBits)
        mDitherAmplitude = 1.0f / static_cast<float>(1 << targetBits);
    }

    /**
     * @brief Procesa un sample con TPDF dithering
     * @param input Sample de entrada (float)
     * @return Sample con dither agregado
     *
     * IMPORTANTE: El output debe ser enviado directamente al DAC
     * sin procesamiento adicional, de lo contrario el dithering
     * pierde efectividad.
     */
    inline float process(float input) {
        // Generar dos samples RPDF independientes
        float dither1 = generateRPDF();
        float dither2 = generateRPDF();

        // TPDF = suma de dos RPDF (distribución triangular)
        float tpdfDither = dither1 + dither2;

        // Agregar dither al input
        return input + tpdfDither;
    }

    /**
     * @brief Reset del generador de ruido
     * @param seed Nueva semilla (útil para crear canales decorrelacionados)
     */
    void reset(uint32_t seed) {
        mRngState = seed;
        if (mRngState == 0) mRngState = 1;  // Evitar estado 0
    }

private:
    /**
     * @brief Genera un sample RPDF (Rectangular PDF)
     * @return Valor aleatorio en rango [-amplitude, +amplitude]
     *
     * Usa Xorshift32 para generar números pseudoaleatorios:
     * - Muy rápido (3 XOR + 3 shifts)
     * - Periodo: 2^32 - 1
     * - Calidad suficiente para dithering de audio
     */
    inline float generateRPDF() {
        // Xorshift32 algorithm
        uint32_t x = mRngState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        mRngState = x;

        // Convertir uint32 [0, 2^32-1] a float [-1, 1]
        // Método: convertir a int32 signed, luego escalar
        int32_t signedX = static_cast<int32_t>(x);
        float normalized = static_cast<float>(signedX) * (1.0f / 2147483648.0f);  // 2^31

        // Escalar a ±amplitude
        return normalized * mDitherAmplitude;
    }

    uint32_t mRngState;       ///< Estado del generador Xorshift32
    float mDitherAmplitude;   ///< Amplitud del dither (±1 LSB)
};

/**
 * @class StereoDitherer
 * @brief Ditherer TPDF estéreo con canales decorrelacionados
 *
 * Mantiene dos instancias de TPDFDitherer con seeds diferentes
 * para garantizar que el dither de L y R no esté correlacionado.
 * Esto previene artefactos de imagen estéreo.
 */
class StereoDitherer {
public:
    /**
     * @brief Constructor
     * @param targetBits Bits de cuantización objetivo (default: 16)
     */
    explicit StereoDitherer(int targetBits = 16)
        : mLeftDitherer(targetBits)
        , mRightDitherer(targetBits)
    {
        // Inicializar con seeds diferentes para decorrelación
        mLeftDitherer.reset(123456);
        mRightDitherer.reset(789012);
    }

    /**
     * @brief Procesa un sample del canal izquierdo
     */
    inline float processLeft(float input) {
        return mLeftDitherer.process(input);
    }

    /**
     * @brief Procesa un sample del canal derecho
     */
    inline float processRight(float input) {
        return mRightDitherer.process(input);
    }

    /**
     * @brief Procesa un buffer estéreo interleaved
     * @param buffer Buffer de entrada/salida (L/R interleaved)
     * @param numFrames Número de frames
     */
    inline void processStereo(float* buffer, int numFrames) {
        for (int i = 0; i < numFrames * 2; i += 2) {
            buffer[i] = mLeftDitherer.process(buffer[i]);       // Left
            buffer[i + 1] = mRightDitherer.process(buffer[i + 1]); // Right
        }
    }

    /**
     * @brief Reset de ambos canales
     */
    void reset() {
        mLeftDitherer.reset(123456);
        mRightDitherer.reset(789012);
    }

private:
    TPDFDitherer mLeftDitherer;   ///< Ditherer para canal izquierdo
    TPDFDitherer mRightDitherer;  ///< Ditherer para canal derecho
};

/**
 * @class RPDFDitherer
 * @brief Ditherer RPDF simple (Rectangular PDF)
 *
 * Implementación más simple de dithering usando solo un generador.
 * Menos efectivo que TPDF pero más rápido.
 *
 * Usado cuando:
 * - Performance es crítica
 * - Target es >20 bits (donde RPDF es suficiente)
 */
class RPDFDitherer {
public:
    explicit RPDFDitherer(int targetBits = 16)
        : mRngState(12345678)
    {
        mDitherAmplitude = 1.0f / static_cast<float>(1 << targetBits);
    }

    inline float process(float input) {
        // Xorshift32
        uint32_t x = mRngState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        mRngState = x;

        // Convertir a float [-1, 1]
        int32_t signedX = static_cast<int32_t>(x);
        float normalized = static_cast<float>(signedX) * (1.0f / 2147483648.0f);

        // Agregar dither
        return input + (normalized * mDitherAmplitude);
    }

    void reset(uint32_t seed) {
        mRngState = seed;
        if (mRngState == 0) mRngState = 1;
    }

private:
    uint32_t mRngState;
    float mDitherAmplitude;
};

#endif // DITHERING_H
