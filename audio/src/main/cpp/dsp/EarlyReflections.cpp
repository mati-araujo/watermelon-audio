#include "EarlyReflections.h"
#include "DSPMath.h"
#include <algorithm>
#include <cmath>

/**
 * Constructor: Inicializa delay line y diffusion network
 */
EarlyReflections::EarlyReflections(int sampleRate)
    : mSampleRate(sampleRate),
      mDelayLine(100.0f, sampleRate)  // Max 100ms para early reflections
{
    initializeDiffusionNetwork();
}

/**
 * Inicializa la red de difusión con 4 allpass filters
 * Tiempos de delay basados en proporciones primas para evitar coloración
 */
void EarlyReflections::initializeDiffusionNetwork() {
    mDiffusionAllpass.clear();

    // Tiempos de allpass en samples (proporciones primas: 7, 11, 13, 17)
    // Escalados para aproximadamente 1-4ms a 48kHz
    const int allpassDelays[4] = {
        static_cast<int>(0.00093f * mSampleRate),  // ~0.93ms (7 * 133 samples @ 48kHz)
        static_cast<int>(0.00147f * mSampleRate),  // ~1.47ms (11 * 133 samples)
        static_cast<int>(0.00173f * mSampleRate),  // ~1.73ms (13 * 133 samples)
        static_cast<int>(0.00227f * mSampleRate)   // ~2.27ms (17 * 133 samples)
    };

    for (int i = 0; i < 4; ++i) {
        mDiffusionAllpass.emplace_back(allpassDelays[i], mSampleRate);
    }
}

/**
 * Procesa un sample individual
 */
float EarlyReflections::process(float input) {
    // RT-SAFE: Cargar parámetros al inicio
    const float diffusionAmount = mDiffusion.load(std::memory_order_acquire);
    const float sizeScale = mSize.load(std::memory_order_acquire);

    // Escribir input al delay line
    mDelayLine.write(input);

    // ===== MULTI-TAP DELAY (Early Reflections) =====
    float earlySum = 0.0f;

    for (int i = 0; i < NUM_TAPS; ++i) {
        // Calcular tiempo de delay escalado por size
        float delayTimeMs = BASE_DELAY_TIMES_MS[i] * sizeScale;

        // Leer del delay line con interpolación
        float delaySamples = delayTimeMs * mSampleRate / 1000.0f;
        float tapOutput = mDelayLine.readInterpolated(delaySamples, DelayLine::Interpolation::LINEAR);

        // Aplicar ganancia del tap
        earlySum += tapOutput * TAP_GAINS[i];
    }

    // Normalizar la suma (dividir por sqrt(NUM_TAPS) para energía constante)
    earlySum *= 0.289f;  // 1/sqrt(12) ≈ 0.289

    // ===== DIFFUSION NETWORK =====
    // Mezclar entre taps puros (diffusion=0) y procesado con allpass (diffusion=1)
    if (diffusionAmount > 0.01f) {
        // Procesar a través de allpass filters
        float diffused = earlySum;
        for (auto& ap : mDiffusionAllpass) {
            diffused = ap.process(diffused);
        }

        // Crossfade entre dry (taps) y wet (diffused)
        earlySum = DSPMath::lerp(earlySum, diffused, diffusionAmount);
    }

    return earlySum;
}

/**
 * Procesa un bloque de samples
 */
void EarlyReflections::processBlock(const float* input, float* output, int numSamples) {
    for (int i = 0; i < numSamples; ++i) {
        output[i] = process(input[i]);
    }
}

/**
 * Establece el control de difusión
 */
void EarlyReflections::setDiffusion(float amount) {
    float clamped = std::clamp(amount, 0.0f, 1.0f);
    mDiffusion.store(clamped, std::memory_order_relaxed);
}

/**
 * Establece el tamaño del espacio
 */
void EarlyReflections::setSize(float size) {
    float clamped = std::clamp(size, 0.5f, 2.0f);
    mSize.store(clamped, std::memory_order_relaxed);
}

/**
 * Cambia el sample rate
 */
void EarlyReflections::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;

    // Reinicializar delay line principal
    mDelayLine.setSampleRate(sampleRate);

    // Reinicializar diffusion network
    initializeDiffusionNetwork();
}

/**
 * Resetea todos los buffers
 */
void EarlyReflections::reset() {
    mDelayLine.clear();

    for (auto& ap : mDiffusionAllpass) {
        ap.reset();
    }
}
