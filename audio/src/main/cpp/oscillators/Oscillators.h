#pragma once
#include "../core/AudioSource.h"
#include "../dsp/ParameterSmoother.h"
#include <cstdlib>
#include <cstdint>

/**
 * @brief PolyBLEP (Polynomial Band-Limited Step) function
 *
 * Reduces aliasing in discontinuous waveforms (square, sawtooth) by
 * smoothing transitions using polynomial interpolation.
 *
 * @param phase Current phase in radians [0, 2π]
 * @param phaseInc Phase increment per sample
 * @return Correction value to apply to the waveform
 */
inline float polyBlep(float phase, float phaseInc) {
    // IMPROVED: Pre-calculate constant to avoid repeated divisions
    constexpr float INV_TWO_PI = 1.0f / (2.0f * M_PI);  // ~0.159155

    // Normalize phase to [0, 1] range using multiplication (faster than division)
    float t = phase * INV_TWO_PI;
    float dt = phaseInc * INV_TWO_PI;

    // Handle discontinuity at phase wrap (0 -> 2π)
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > (1.0f - dt)) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// 1. SINE WAVE (Suave)
class SineOscillator : public AudioSource {
public:
    SineOscillator() {
        // Smoothers se configurarán cuando se llame setSampleRate()
    }

    void setSampleRate(int32_t sampleRate) override {
        AudioSource::setSampleRate(sampleRate);
        // Suavizado de 5ms para frecuencia (evita glitches en cambios de pitch)
        mFreqSmoother.setSmoothingTime(5.0f, static_cast<float>(sampleRate));
        // Suavizado de 10ms para amplitud (evita clicks en cambios de volumen)
        mAmpSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    }

    void render(float* audioData, int32_t numFrames) override {
        float targetFreq = mFrequency.load();
        float targetAmp = mAmplitude.load();

        for (int i = 0; i < numFrames; ++i) {
            // Aplicar suavizado sample por sample para transiciones ultra-suaves
            float freq = mFreqSmoother.process(targetFreq);
            float amp = mAmpSmoother.process(targetAmp);
            float phaseIncrement = (freq * 2.0f * M_PI) / mSampleRate;

            float sample = sin(mPhase) * amp;
            // Generar stereo duplicando el mono
            audioData[i * 2] = sample;      // Left
            audioData[i * 2 + 1] = sample;  // Right
            mPhase += phaseIncrement;
            if (mPhase >= 2.0f * M_PI) mPhase = fmodf(mPhase, 2.0f * M_PI);
        }
    }

private:
    ParameterSmoother mFreqSmoother;
    ParameterSmoother mAmpSmoother;
};

// 2. SQUARE WAVE (Agresiva, tipo Gameboy/Chiptune)
// IMPROVED: Now using PolyBLEP to reduce aliasing and ParameterSmoother for smooth transitions
class SquareOscillator : public AudioSource {
public:
    SquareOscillator() {
        // Smoothers se configurarán cuando se llame setSampleRate()
    }

    void setSampleRate(int32_t sampleRate) override {
        AudioSource::setSampleRate(sampleRate);
        // Suavizado de 5ms para frecuencia (evita glitches en cambios de pitch)
        mFreqSmoother.setSmoothingTime(5.0f, static_cast<float>(sampleRate));
        // Suavizado de 10ms para amplitud (evita clicks en cambios de volumen)
        mAmpSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    }

    void render(float* audioData, int32_t numFrames) override {
        float targetFreq = mFrequency.load();
        float targetAmp = mAmplitude.load();

        for (int i = 0; i < numFrames; ++i) {
            // Aplicar suavizado sample por sample para transiciones ultra-suaves
            float freq = mFreqSmoother.process(targetFreq);
            float amp = mAmpSmoother.process(targetAmp);
            float phaseIncrement = (freq * 2.0f * M_PI) / mSampleRate;

            // Naive square wave
            float sample = (mPhase < M_PI ? 1.0f : -1.0f);

            // Apply PolyBLEP correction at discontinuities
            // Discontinuity at 0 (rising edge)
            sample -= polyBlep(mPhase, phaseIncrement);
            // Discontinuity at π (falling edge)
            sample += polyBlep(fmodf(mPhase + M_PI, 2.0f * M_PI), phaseIncrement);

            sample *= amp;

            // Generar stereo duplicando el mono
            audioData[i * 2] = sample;      // Left
            audioData[i * 2 + 1] = sample;  // Right

            mPhase += phaseIncrement;
            if (mPhase >= 2.0f * M_PI) mPhase = fmodf(mPhase, 2.0f * M_PI);
        }
    }

private:
    ParameterSmoother mFreqSmoother;
    ParameterSmoother mAmpSmoother;
};

// 3. SAWTOOTH WAVE (Rica en armónicos, zumbido)
// IMPROVED: Now using PolyBLEP to reduce aliasing and ParameterSmoother for smooth transitions
class SawtoothOscillator : public AudioSource {
public:
    SawtoothOscillator() {
        // Smoothers se configurarán cuando se llame setSampleRate()
    }

    void setSampleRate(int32_t sampleRate) override {
        AudioSource::setSampleRate(sampleRate);
        // Suavizado de 5ms para frecuencia (evita glitches en cambios de pitch)
        mFreqSmoother.setSmoothingTime(5.0f, static_cast<float>(sampleRate));
        // Suavizado de 10ms para amplitud (evita clicks en cambios de volumen)
        mAmpSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    }

    void render(float* audioData, int32_t numFrames) override {
        float targetFreq = mFrequency.load();
        float targetAmp = mAmplitude.load();

        for (int i = 0; i < numFrames; ++i) {
            // Aplicar suavizado sample por sample para transiciones ultra-suaves
            float freq = mFreqSmoother.process(targetFreq);
            float amp = mAmpSmoother.process(targetAmp);
            float phaseIncrement = (freq * 2.0f * M_PI) / mSampleRate;

            // Naive sawtooth: ramp from -1 to 1
            float t = mPhase / (2.0f * M_PI);
            float sample = 2.0f * t - 1.0f;

            // Apply PolyBLEP correction at discontinuity (phase wrap at 2π)
            sample -= polyBlep(mPhase, phaseIncrement);

            sample *= amp;

            // Generar stereo duplicando el mono
            audioData[i * 2] = sample;      // Left
            audioData[i * 2 + 1] = sample;  // Right

            mPhase += phaseIncrement;
            if (mPhase >= 2.0f * M_PI) mPhase = fmodf(mPhase, 2.0f * M_PI);
        }
    }

private:
    ParameterSmoother mFreqSmoother;
    ParameterSmoother mAmpSmoother;
};

// 4. TRIANGLE WAVE (Aflautada pero con bordes)
class TriangleOscillator : public AudioSource {
public:
    TriangleOscillator() {
        // Smoothers se configurarán cuando se llame setSampleRate()
    }

    void setSampleRate(int32_t sampleRate) override {
        AudioSource::setSampleRate(sampleRate);
        // Suavizado de 5ms para frecuencia (evita glitches en cambios de pitch)
        mFreqSmoother.setSmoothingTime(5.0f, static_cast<float>(sampleRate));
        // Suavizado de 10ms para amplitud (evita clicks en cambios de volumen)
        mAmpSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    }

    void render(float* audioData, int32_t numFrames) override {
        float targetFreq = mFrequency.load();
        float targetAmp = mAmplitude.load();

        for (int i = 0; i < numFrames; ++i) {
            // Aplicar suavizado sample por sample para transiciones ultra-suaves
            float freq = mFreqSmoother.process(targetFreq);
            float amp = mAmpSmoother.process(targetAmp);
            float phaseIncrement = (freq * 2.0f * M_PI) / mSampleRate;

            // Valor absoluto de la Sawtooth ajustada
            float value = 2.0f * (mPhase / (2.0f * M_PI)) - 1.0f;
            float sample = (2.0f * std::abs(value) - 1.0f) * amp;
            // Generar stereo duplicando el mono
            audioData[i * 2] = sample;      // Left
            audioData[i * 2 + 1] = sample;  // Right

            mPhase += phaseIncrement;
            if (mPhase >= 2.0f * M_PI) mPhase = fmodf(mPhase, 2.0f * M_PI);
        }
    }

private:
    ParameterSmoother mFreqSmoother;
    ParameterSmoother mAmpSmoother;
};

// 5. WHITE NOISE (Aleatorio, percusivo/efectos)
// FIXED: Usar xorshift32 en vez de rand() - thread-safe y mejor calidad para audio
class NoiseGenerator : public AudioSource {
public:
    NoiseGenerator() : mRngState(12345) {
        // Smoother se configurará cuando se llame setSampleRate()
    }

    void setSampleRate(int32_t sampleRate) override {
        AudioSource::setSampleRate(sampleRate);
        // Suavizado de 10ms para amplitud (evita clicks en cambios de volumen)
        mAmpSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    }

    void render(float* audioData, int32_t numFrames) override {
        float targetAmp = mAmplitude.load();

        for (int i = 0; i < numFrames; ++i) {
            // Aplicar suavizado sample por sample para transiciones ultra-suaves
            float amp = mAmpSmoother.process(targetAmp);

            // Xorshift32 algorithm para canal Left
            uint32_t x = mRngState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            mRngState = x;

            // IMPROVED: Conversión más precisa uint32 → float [-1, 1]
            // Usar int32_t signed para evitar sesgo de cuantización
            // 0xFFFFFFFF (impar) genera sesgo, mejor usar 2^32 par
            constexpr float INV_UINT32_MAX = 1.0f / 4294967296.0f;  // 1 / 2^32
            int32_t signedX = static_cast<int32_t>(x);
            float randomL = static_cast<float>(signedX) * (INV_UINT32_MAX * 2.0f);
            audioData[i * 2] = randomL * amp;  // Left

            // Segundo sample para Right (descorrelacionado)
            x = mRngState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            mRngState = x;
            signedX = static_cast<int32_t>(x);
            float randomR = static_cast<float>(signedX) * (INV_UINT32_MAX * 2.0f);
            audioData[i * 2 + 1] = randomR * amp;  // Right
        }
    }

private:
    uint32_t mRngState;  // Estado del RNG thread-local
    ParameterSmoother mAmpSmoother;
};

// ========== FILTROS PARA PROCESAMIENTO DE AUDIO ==========

/**
 * @brief Filtro Biquad genérico (2nd order IIR filter)
 *
 * Implementa diferentes tipos de filtros usando la estructura biquad estándar.
 * Thread-safe cuando se usa en un solo thread de audio.
 */
class BiquadFilter {
public:
    BiquadFilter() : b0(1.0f), b1(0.0f), b2(0.0f), a1(0.0f), a2(0.0f),
                     z1(0.0f), z2(0.0f) {}

    /**
     * @brief Configura el filtro como pasa banda (bandpass)
     * @param centerFreq Frecuencia central en Hz
     * @param q Factor Q (ancho de banda inverso). Valores típicos: 0.5-20
     *          Q más bajo = banda más ancha, Q más alto = banda más estrecha
     * @param sampleRate Sample rate en Hz
     */
    void setBandpass(float centerFreq, float q, float sampleRate) {
        // Prevenir divisiones por cero y valores inválidos
        if (q <= 0.0f) q = 0.707f;  // Default Q
        if (centerFreq <= 0.0f) centerFreq = 1000.0f;
        if (sampleRate <= 0.0f) sampleRate = 48000.0f;

        // Limitar frecuencia a Nyquist
        float nyquist = sampleRate * 0.5f;
        if (centerFreq >= nyquist) centerFreq = nyquist * 0.99f;

        // Calcular coeficientes del filtro biquad bandpass
        float w0 = 2.0f * M_PI * centerFreq / sampleRate;
        float sinW0 = sinf(w0);
        float cosW0 = cosf(w0);
        float alpha = sinW0 / (2.0f * q);

        // Coeficientes normalizados
        float a0 = 1.0f + alpha;
        b0 = alpha / a0;
        b1 = 0.0f;
        b2 = -alpha / a0;
        a1 = -2.0f * cosW0 / a0;
        a2 = (1.0f - alpha) / a0;
    }

    /**
     * @brief Procesa un sample de audio
     * @param input Sample de entrada
     * @return Sample filtrado
     */
    inline float process(float input) {
        // Direct Form II Transposed (más eficiente y estable numéricamente)
        float output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;

        // Denormal protection: flush very small values to zero
        constexpr float DENORMAL_THRESHOLD = 1e-20f;
        if (std::abs(z1) < DENORMAL_THRESHOLD) z1 = 0.0f;
        if (std::abs(z2) < DENORMAL_THRESHOLD) z2 = 0.0f;

        return output;
    }

    /**
     * @brief Resetea el estado interno del filtro
     */
    void reset() {
        z1 = 0.0f;
        z2 = 0.0f;
    }

private:
    // Coeficientes del filtro
    float b0, b1, b2;  // Feedforward
    float a1, a2;      // Feedback (a0 siempre normalizado a 1.0)

    // Estado interno (memoria del filtro)
    float z1, z2;
};

// 6. BAND-LIMITED NOISE (Ruido filtrado para efectos especiales)
/**
 * @brief Generador de ruido de banda estrecha usando filtros pasa banda
 *
 * Toma ruido blanco y lo filtra a través de un filtro bandpass para crear
 * ruido en una banda de frecuencias específica. Útil para:
 * - Efectos de viento
 * - Texturas atmosféricas
 * - Síntesis granular
 * - Ruido coloreado
 */
class BandLimitedNoiseGenerator : public AudioSource {
public:
    BandLimitedNoiseGenerator() : mRngState(54321) {
        // Smoother se configurará cuando se llame setSampleRate()
        // Filtros se configurarán cuando se establezca el sample rate
    }

    void setSampleRate(int32_t sampleRate) override {
        AudioSource::setSampleRate(sampleRate);

        // Suavizado de 10ms para amplitud (evita clicks en cambios de volumen)
        mAmpSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));

        // Suavizado de 20ms para frecuencia central (evita cambios bruscos del filtro)
        mCenterFreqSmoother.setSmoothingTime(20.0f, static_cast<float>(sampleRate));

        // Configurar filtros iniciales
        // Frecuencia central predeterminada: 1000 Hz, Q=5 (banda estrecha)
        updateFilters(1000.0f, static_cast<float>(sampleRate));
    }

    void render(float* audioData, int32_t numFrames) override {
        float targetAmp = mAmplitude.load();
        // Usar frecuencia para controlar la frecuencia central del filtro
        float targetCenterFreq = mFrequency.load();
        // Limitar frecuencia central entre 50 Hz y 10 kHz
        targetCenterFreq = std::max(50.0f, std::min(10000.0f, targetCenterFreq));

        // CRITICAL: Update filters ONCE at start of render, not in tight loop
        // IMPROVED: Threshold proporcional a la frecuencia (5% de cambio)
        float smoothedCenterFreq = mCenterFreqSmoother.process(targetCenterFreq);
        float threshold = targetCenterFreq * 0.05f;  // 5% de la frecuencia
        threshold = std::max(threshold, 5.0f);  // Mínimo 5Hz
        if (std::abs(smoothedCenterFreq - mCurrentCenterFreq) > threshold) {
            updateFilters(smoothedCenterFreq, static_cast<float>(mSampleRate));
        }

        for (int i = 0; i < numFrames; ++i) {
            // Aplicar suavizado sample por sample
            float amp = mAmpSmoother.process(targetAmp);

            // Generar ruido blanco para canal Left
            uint32_t x = mRngState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            mRngState = x;

            // IMPROVED: Conversión más precisa uint32 → float
            constexpr float INV_UINT32_MAX = 1.0f / 4294967296.0f;
            int32_t signedX = static_cast<int32_t>(x);
            float randomL = static_cast<float>(signedX) * (INV_UINT32_MAX * 2.0f);

            // Aplicar filtro bandpass
            float filteredL = mFilterL.process(randomL);
            // Compensar pérdida de ganancia del filtro (típicamente ~6dB)
            filteredL *= 3.0f;
            audioData[i * 2] = filteredL * amp;  // Left

            // Generar ruido blanco para canal Right (descorrelacionado)
            x = mRngState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            mRngState = x;
            signedX = static_cast<int32_t>(x);
            float randomR = static_cast<float>(signedX) * (INV_UINT32_MAX * 2.0f);

            // Aplicar filtro bandpass
            float filteredR = mFilterR.process(randomR);
            filteredR *= 3.0f;
            audioData[i * 2 + 1] = filteredR * amp;  // Right
        }
    }

private:
    /**
     * @brief Actualiza los coeficientes de los filtros
     */
    void updateFilters(float centerFreq, float sampleRate) {
        // Q factor: controla el ancho de banda
        // Q=5 da una banda relativamente estrecha pero musical
        // Para banda más estrecha usar Q=10 o superior
        const float Q = 5.0f;

        mFilterL.setBandpass(centerFreq, Q, sampleRate);
        mFilterR.setBandpass(centerFreq, Q, sampleRate);
        mCurrentCenterFreq = centerFreq;
    }

    uint32_t mRngState;  // Estado del RNG thread-local
    ParameterSmoother mAmpSmoother;
    ParameterSmoother mCenterFreqSmoother;
    BiquadFilter mFilterL;  // Filtro para canal izquierdo
    BiquadFilter mFilterR;  // Filtro para canal derecho
    float mCurrentCenterFreq = 1000.0f;
};
