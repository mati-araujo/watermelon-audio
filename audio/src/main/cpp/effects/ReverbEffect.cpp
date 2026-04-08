#include "ReverbEffect.h"
#include "EffectDefaults.h"
#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>

/**
 * Constructor: Inicializa reverb profesional con DSP infrastructure
 */
ReverbEffect::ReverbEffect()
    : mSampleRate(DEFAULT_SAMPLE_RATE),
      preDelayLine(100.0f, mSampleRate),        // Max 100ms pre-delay
      lowCutFilter(mSampleRate),
      highCutFilter(mSampleRate),
      stereoProcessor(mSampleRate),
      earlyReflections(mSampleRate)             // Early reflections (Iteración 2)
{
    // ===== LATE REVERB (Freeverb - comb filters) =====
    float baseCombDelays[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
    for (int i = 0; i < 4; ++i) {
        int delaySamples = static_cast<int>(baseCombDelays[i] * mSampleRate);
        combBuffers[i].resize(delaySamples, 0.0f);
        combPos[i].store(0, std::memory_order_relaxed);
        combGains[i].store(0.0f, std::memory_order_relaxed);
    }

    // ===== LATE REVERB (Freeverb - allpass filters) =====
    float baseAllpassDelays[2] = {0.005f, 0.0017f};
    for (int i = 0; i < 2; ++i) {
        int delaySamples = static_cast<int>(baseAllpassDelays[i] * mSampleRate);
        allpassBuffers[i].resize(delaySamples, 0.0f);
        allpassPos[i].store(0, std::memory_order_relaxed);
    }
    allpassGain = 0.7f;

    // ===== TONE CONTROL (Filters) =====
    // Inicializar con valores por defecto
    lowCutFilter.setHighpass(lowCutFreq.load(), 0.707f);   // 80Hz HPF
    highCutFilter.setLowpass(highCutFreq.load(), 0.707f);  // 12kHz LPF

    // ===== STEREO PROCESSOR =====
    stereoProcessor.setWidth(stereoWidth.load());

    // ===== EARLY REFLECTIONS (Iteración 2) =====
    earlyReflections.setDiffusion(diffusion.load());
    earlyReflections.setSize(size.load());

    // ===== MODULATION LFOs (Iteración 3) =====
    // Inicializar 4 LFOs con fases desplazadas para efecto más rico
    // Fases: 0°, 90°, 180°, 270° (proporciona decorrelación entre los combs)
    const float PI = 3.14159265359f;
    for (int i = 0; i < 4; ++i) {
        modulationLFOs[i].setRate(modRate.load());
        modulationLFOs[i].setWaveform(LFO::Waveform::SINE);  // Sine suave para shimmer
        modulationLFOs[i].setPhaseOffset(i * PI / 2.0f);     // 0°, 90°, 180°, 270°
    }

    // ===== COMPUTE INITIAL PARAMETERS =====
    updateParameters();
}

// ============================================================================
// SETTERS - PARÁMETROS BÁSICOS (0-2)
// ============================================================================

void ReverbEffect::setDecay(float d) {
    decay.store(std::clamp(d, 0.1f, 5.0f), std::memory_order_relaxed);
    updateParameters();
}

void ReverbEffect::setSize(float s) {
    float clamped = std::clamp(s, 0.5f, 2.0f);
    size.store(clamped, std::memory_order_relaxed);

    // Actualizar el tamaño de early reflections (Iteración 2)
    earlyReflections.setSize(clamped);

    updateParameters();
}

void ReverbEffect::setMix(float m) {
    mix.store(std::clamp(m, 0.0f, 1.0f), std::memory_order_relaxed);
}

// ============================================================================
// SETTERS - PARÁMETROS NUEVOS (Iteración 1)
// ============================================================================

void ReverbEffect::setPreDelay(float delayMs) {
    preDelayMs.store(std::clamp(delayMs, 0.0f, 100.0f), std::memory_order_relaxed);
}

void ReverbEffect::setDamping(float amount) {
    damping.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
}

void ReverbEffect::setDiffusion(float amount) {
    float clamped = std::clamp(amount, 0.0f, 1.0f);
    diffusion.store(clamped, std::memory_order_relaxed);

    // Actualizar el componente EarlyReflections
    earlyReflections.setDiffusion(clamped);
}

void ReverbEffect::setStereoWidth(float width) {
    float clamped = std::clamp(width, 0.0f, 1.0f);
    stereoWidth.store(clamped, std::memory_order_relaxed);

    // Actualizar el StereoDecorrelator
    stereoProcessor.setWidth(clamped);
}

void ReverbEffect::setEarlyLateMix(float mix) {
    earlyLateMix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void ReverbEffect::setModDepth(float depth) {
    modDepth.store(std::clamp(depth, 0.0f, 1.0f), std::memory_order_relaxed);
}

void ReverbEffect::setModRate(float rateHz) {
    float clamped = std::clamp(rateHz, 0.1f, 5.0f);
    modRate.store(clamped, std::memory_order_relaxed);

    // Actualizar la velocidad de todos los LFOs
    for (int i = 0; i < 4; ++i) {
        modulationLFOs[i].setRate(clamped);
    }
}

void ReverbEffect::setLowCut(float frequency) {
    float clamped = std::clamp(frequency, 20.0f, 500.0f);
    lowCutFreq.store(clamped, std::memory_order_relaxed);

    // Actualizar el filtro HPF
    lowCutFilter.setHighpass(clamped, 0.707f);
}

void ReverbEffect::setHighCut(float frequency) {
    float clamped = std::clamp(frequency, 1000.0f, 20000.0f);
    highCutFreq.store(clamped, std::memory_order_relaxed);

    // Actualizar el filtro LPF
    highCutFilter.setLowpass(clamped, 0.707f);
}

// ============================================================================
// PROCESO PRINCIPAL - Arquitectura Profesional
// ============================================================================

/**
 * Nueva arquitectura de procesamiento (Iteración 2):
 *   Input → Pre-Delay → [Early Reflections + Late Reverb] → Tone Control → Stereo → Output
 */
void ReverbEffect::process(float* input, float* output, int numFrames) {
    // RT-SAFE: Cargar parámetros atómicamente al inicio
    const float wet = mix.load(std::memory_order_acquire);
    const float dry = 1.0f - wet;
    const float preDelayAmount = preDelayMs.load(std::memory_order_acquire);
    const float earlyLateMixAmount = earlyLateMix.load(std::memory_order_acquire);

    for (int i = 0; i < numFrames * 2; i += 2) {
        const float inL = input[i];
        const float inR = input[i + 1];
        const float monoIn = (inL + inR) * 0.5f;

        // ===== 1. PRE-DELAY =====
        // Convert ms to samples for DelayLine::process
        float delaySamples = preDelayAmount * mSampleRate / 1000.0f;
        float delayed = preDelayLine.process(monoIn, delaySamples);

        // ===== 2a. EARLY REFLECTIONS (Iteración 2) =====
        float earlyOut = earlyReflections.process(delayed);

        // ===== 2b. LATE REVERB (Freeverb: Comb + Allpass) =====
        float lateOut = processLateReverb(delayed);

        // ===== 2c. MIX EARLY + LATE =====
        // earlyLateMix: 0 = solo late reverb, 1 = solo early reflections
        float reverbMono = DSPMath::lerp(lateOut, earlyOut, earlyLateMixAmount);

        // ===== 3. TONE CONTROL (HPF → LPF) =====
        reverbMono = lowCutFilter.process(reverbMono);
        reverbMono = highCutFilter.process(reverbMono);

        // ===== 4. STEREO DECORRELATION =====
        auto [reverbL, reverbR] = stereoProcessor.process(reverbMono);

        // ===== 5. MIX WET/DRY =====
        output[i] = dry * inL + wet * reverbL;
        output[i + 1] = dry * inR + wet * reverbR;

        // ===== 6. SAFETY: Protect against NaN/Inf =====
        // Check for invalid values that can cause glitches or crashes
        if (!std::isfinite(output[i])) output[i] = 0.0f;
        if (!std::isfinite(output[i + 1])) output[i + 1] = 0.0f;

        // CRITICAL FIX: Soft clip en lugar de hard clip (más musical)
        // Arctan soft clipper con headroom de ±2.5
        // Fórmula: atan(x * 0.8) * 1.273 donde 1.273 ≈ 2/π
        output[i] = std::atan(output[i] * 0.8f) * 1.273f;
        output[i + 1] = std::atan(output[i + 1] * 0.8f) * 1.273f;

        // Hard limit como safety final (solo para valores extremos)
        output[i] = std::clamp(output[i], -1.0f, 1.0f);
        output[i + 1] = std::clamp(output[i + 1], -1.0f, 1.0f);
    }
}

// ============================================================================
// HELPERS
// ============================================================================

/**
 * Procesa late reverb (Freeverb algorithm: 4 comb + 2 allpass)
 * Con modulación opcional para shimmer effect (Iteración 3)
 *
 * @param monoInput Señal mono de entrada (ya con pre-delay aplicado)
 * @return Señal mono reverberada (sin tone control ni stereo)
 */
float ReverbEffect::processLateReverb(float monoInput) {
    // RT-SAFE: Cargar parámetros al inicio
    float combGainsLocal[4];
    for (int j = 0; j < 4; ++j) {
        combGainsLocal[j] = combGains[j].load(std::memory_order_acquire);
    }
    const float dampAmount = damping.load(std::memory_order_acquire);
    const float modDepthAmount = modDepth.load(std::memory_order_acquire);

    // ===== COMB FILTERS (4 parallel) con modulación opcional =====
    float combOut = 0.0f;
    for (int j = 0; j < 4; ++j) {
        int pos = combPos[j].load(std::memory_order_relaxed);

        // ===== MODULACIÓN (Iteración 3) =====
        // Si modDepth > 0, modular el delay time para shimmer effect
        float delayed;
        if (modDepthAmount > 0.01f) {
            // LFO genera valores [-1, 1]
            float lfoValue = modulationLFOs[j].process();

            // Convertir a desplazamiento en samples (±2 samples máximo)
            // Esto crea un shimmer sutil sin detuning excesivo
            float modOffset = lfoValue * modDepthAmount * 2.0f;

            // Leer con interpolación para modulación suave
            int bufferSize = static_cast<int>(combBuffers[j].size());
            float readPos = static_cast<float>(pos) + modOffset;

            // Wrap around usando módulo eficiente (evitar while loops en RT thread)
            if (bufferSize > 0) {
                readPos = std::fmod(readPos, static_cast<float>(bufferSize));
                if (readPos < 0.0f) readPos += bufferSize;
            }

            // Interpolación lineal para lectura fraccionaria con bounds checking
            int pos1 = static_cast<int>(readPos);
            pos1 = std::clamp(pos1, 0, bufferSize - 1);
            int pos2 = (pos1 + 1) % bufferSize;
            float frac = readPos - pos1;
            frac = std::clamp(frac, 0.0f, 1.0f);

            delayed = combBuffers[j][pos1] * (1.0f - frac) + combBuffers[j][pos2] * frac;
        } else {
            // Sin modulación: lectura directa
            delayed = combBuffers[j][pos];
        }

        // One-pole lowpass damping
        float filtered = delayed * (1.0f - dampAmount) + combPrevFiltered[j] * dampAmount;
        combOut += filtered;

        // Write to buffer (input + feedback)
        combBuffers[j][pos] = monoInput + filtered * combGainsLocal[j];

        // Advance position
        int newPos = (pos + 1) % static_cast<int>(combBuffers[j].size());
        combPos[j].store(newPos, std::memory_order_relaxed);

        // Update filtered state with denormal protection
        combPrevFiltered[j] = filtered;
        if (std::abs(combPrevFiltered[j]) < 1e-20f) {
            combPrevFiltered[j] = 0.0f;
        }
    }

    combOut *= 0.25f; // Average of 4 combs

    // ===== ALLPASS FILTERS (2 serial) =====
    float allpassIn = combOut;
    for (int j = 0; j < 2; ++j) {
        // Read from circular buffer
        int pos = allpassPos[j].load(std::memory_order_relaxed);
        float delayed = allpassBuffers[j][pos];

        // Allpass formula: out = -g*in + delayed + g*out
        float out = delayed - allpassGain * allpassIn;

        // Write to buffer
        allpassBuffers[j][pos] = allpassIn + allpassGain * out;

        // Advance position
        int newPos = (pos + 1) % static_cast<int>(allpassBuffers[j].size());
        allpassPos[j].store(newPos, std::memory_order_relaxed);

        allpassIn = out;
    }

    return allpassIn;
}

/**
 * Actualiza parámetros derivados (comb gains basados en decay y size)
 */
void ReverbEffect::updateParameters() {
    const float d = decay.load(std::memory_order_acquire);
    const float s = size.load(std::memory_order_acquire);

    float baseDelays[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
    for (int i = 0; i < 4; ++i) {
        float delay = baseDelays[i] * s;
        // Ganancia para -60dB decay en tiempo 'd'
        float newGain = powf(10.0f, -3.0f * delay / d);
        combGains[i].store(newGain, std::memory_order_release);
    }
}

// ============================================================================
// PARAMETER INTERFACE (12 parámetros totales)
// ============================================================================

void ReverbEffect::setParam(int paramId, float value) {
    switch(paramId) {
        // Parámetros básicos (0-2)
        case 0: setDecay(value); break;
        case 1: setSize(value); break;
        case 2: setMix(value); break;

        // Parámetros nuevos - Iteración 1 (3, 4, 6, 10, 11)
        case 3: setPreDelay(value); break;
        case 4: setDamping(value); break;
        case 6: setStereoWidth(value); break;
        case 10: setLowCut(value); break;
        case 11: setHighCut(value); break;

        // Parámetros activos - Iteración 2 (5, 7)
        case 5: setDiffusion(value); break;     // Diffusion (ACTIVO en Iter 2)
        case 7: setEarlyLateMix(value); break;  // Early/Late Mix (ACTIVO en Iter 2)

        // Parámetros activos - Iteración 3 (8, 9)
        case 8: setModDepth(value); break;      // Mod Depth (ACTIVO en Iter 3)
        case 9: setModRate(value); break;       // Mod Rate (ACTIVO en Iter 3)
    }
}

float ReverbEffect::getParam(int paramId) {
    switch(paramId) {
        // Parámetros básicos (0-2)
        case 0: return decay.load(std::memory_order_relaxed);
        case 1: return size.load(std::memory_order_relaxed);
        case 2: return mix.load(std::memory_order_relaxed);

        // Parámetros nuevos - Iteración 1 (3, 4, 6, 10, 11)
        case 3: return preDelayMs.load(std::memory_order_relaxed);
        case 4: return damping.load(std::memory_order_relaxed);
        case 6: return stereoWidth.load(std::memory_order_relaxed);
        case 10: return lowCutFreq.load(std::memory_order_relaxed);
        case 11: return highCutFreq.load(std::memory_order_relaxed);

        // Parámetros reservados - Iteración 2 y 3 (5, 7, 8, 9)
        case 5: return diffusion.load(std::memory_order_relaxed);
        case 7: return earlyLateMix.load(std::memory_order_relaxed);
        case 8: return modDepth.load(std::memory_order_relaxed);
        case 9: return modRate.load(std::memory_order_relaxed);
    }
    return 0.0f;
}

// ============================================================================
// SAMPLE RATE CHANGES
// ============================================================================

void ReverbEffect::setSampleRate(int sampleRate) {
    // Store the new sample rate
    mSampleRate = sampleRate;

    // ===== RESIZE LATE REVERB BUFFERS =====
    float baseCombDelays[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
    for (int i = 0; i < 4; ++i) {
        int delaySamples = static_cast<int>(baseCombDelays[i] * mSampleRate);
        combBuffers[i].clear();
        combBuffers[i].resize(delaySamples, 0.0f);
        combPos[i].store(0, std::memory_order_relaxed);
    }

    float baseAllpassDelays[2] = {0.005f, 0.0017f};
    for (int i = 0; i < 2; ++i) {
        int delaySamples = static_cast<int>(baseAllpassDelays[i] * mSampleRate);
        allpassBuffers[i].clear();
        allpassBuffers[i].resize(delaySamples, 0.0f);
        allpassPos[i].store(0, std::memory_order_relaxed);
    }

    // ===== UPDATE DSP COMPONENTS =====
    // Iteración 1
    preDelayLine.setSampleRate(sampleRate);
    lowCutFilter.setSampleRate(sampleRate);
    highCutFilter.setSampleRate(sampleRate);
    stereoProcessor.setSampleRate(sampleRate);

    // Iteración 2
    earlyReflections.setSampleRate(sampleRate);

    // Iteración 3
    for (int i = 0; i < 4; ++i) {
        modulationLFOs[i].setSampleRate(static_cast<float>(sampleRate));
    }

    // ===== RECALCULATE FILTERS WITH NEW SAMPLE RATE =====
    lowCutFilter.setHighpass(lowCutFreq.load(std::memory_order_relaxed), 0.707f);
    highCutFilter.setLowpass(highCutFreq.load(std::memory_order_relaxed), 0.707f);

    // ===== RECALCULATE COMB GAINS =====
    updateParameters();
}
