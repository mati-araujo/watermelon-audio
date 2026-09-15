/**
 * SoundFontChorus.h — REQ-040 S2: el chorus FIJO del SoundFont.
 *
 * Mismo estatus que `SoundFontReverb`: es el sonido del font (`chorusEffectsSend`, SF2 §8.1.2
 * #15), no un efecto del usuario, y los parámetros son los default de FluidSynth 2.6.0
 * (`synth.chorus.*`: 3 voces · level 2,0 · 0,3 Hz · 8 ms de profundidad · senoidal), que son los
 * de la referencia `-C 1` del spec-test (prueba #18).
 *
 * La estructura es la de un chorus de N líneas moduladas sobre una entrada MONO: cada voz lee la
 * línea con un retardo que un LFO senoidal mueve alrededor de un centro, las voces reparten sus
 * fases uniformemente y se alternan a izquierda y derecha. FluidSynth (LGPL) no se copia; lo que
 * se toma son los parámetros y la forma general. Dos constantes NO salen del spec ni de la
 * referencia por nombre y se declaran acá para que S3 las mida contra el render `-C 1`:
 * `kCenterDelayMs` y el reparto de `kLevel` entre voces.
 *
 * RT: `addChorusWetFromMono` es aritmética sobre una línea que `prepare()` alocó. Nombres únicos.
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace wma {

class SoundFontChorus {
public:
    static constexpr int kVoices = 3;
    static constexpr float kLevel = 2.0f;
    static constexpr float kSpeedHz = 0.3f;
    static constexpr float kDepthMs = 8.0f;       // profundidad TOTAL (pico a pico) de cada voz
    static constexpr float kCenterDelayMs = 16.0f; // el centro alrededor del que modula cada voz

    /// Thread de CONTROL: aloca.
    void prepare(float sampleRate) {
        mRate = sampleRate;
        mCenter = kCenterDelayMs * 0.001f * sampleRate;
        mHalfDepth = 0.5f * kDepthMs * 0.001f * sampleRate;
        const std::size_t maxDelay = static_cast<std::size_t>(std::ceil(mCenter + mHalfDepth)) + 4;
        mLine.assign(maxDelay, 0.0f);
        mWrite = 0;
        for (int v = 0; v < kVoices; ++v) mPhase[v] = static_cast<float>(v) / kVoices;
        mPhaseInc = kSpeedHz / sampleRate;
        // Cada voz aporta level / N: con N voces en fase el wet vale `level × entrada`.
        mVoiceGain = kLevel / static_cast<float>(kVoices);
        clearChorusTail();
        mPrepared = true;
    }

    /// RT. Vacía la línea y deja los LFO en su fase inicial.
    void clearChorusTail() noexcept {
        for (float& x : mLine) x = 0.0f;
        mWrite = 0;
        for (int v = 0; v < kVoices; ++v) mPhase[v] = static_cast<float>(v) / kVoices;
    }

    /// RT. Suma a `stereoOut` (entrelazado) el wet de `monoIn`. Las voces pares van a la
    /// izquierda y las impares a la derecha. El LFO (0,3 Hz) se evalúa por BLOQUE, dos veces por
    /// voz (al principio y al final), y el retardo se interpola linealmente en el medio: a esa
    /// velocidad el error contra el seno por muestra es < 1e-4 muestras, y ahorra tres `sin`
    /// por muestra en el thread de audio.
    void addChorusWetFromMono(const float* monoIn, float* stereoOut, int frames) noexcept {
        if (!mPrepared || frames <= 0) return;
        float* line = mLine.data();
        const std::size_t n = mLine.size();
        for (int v = 0; v < kVoices; ++v) {
            const float phaseEnd = mPhase[v] + mPhaseInc * static_cast<float>(frames);
            const float d0 = mCenter + std::sin(6.28318530718f * mPhase[v]) * mHalfDepth;
            const float d1 = mCenter + std::sin(6.28318530718f * phaseEnd) * mHalfDepth;
            mDelayStart[v] = d0;
            mDelayStep[v] = (d1 - d0) / static_cast<float>(frames);
            mPhase[v] = phaseEnd - std::floor(phaseEnd);
        }
        std::size_t w = mWrite;
        for (int i = 0; i < frames; ++i) {
            line[w] = monoIn[i];
            float outL = 0.0f, outR = 0.0f;
            for (int v = 0; v < kVoices; ++v) {
                const float delay = mDelayStart[v] + mDelayStep[v] * static_cast<float>(i);
                float rp = static_cast<float>(w) - delay;
                if (rp < 0.0f) rp += static_cast<float>(n);
                std::size_t i0 = static_cast<std::size_t>(rp);
                if (i0 >= n) i0 -= n;
                std::size_t i1 = i0 + 1;
                if (i1 >= n) i1 = 0;
                const float frac = rp - static_cast<float>(static_cast<std::size_t>(rp));
                const float sample = line[i0] + (line[i1] - line[i0]) * frac;
                if ((v & 1) == 0) outL += sample; else outR += sample;
            }
            stereoOut[2 * i] += outL * mVoiceGain;
            stereoOut[2 * i + 1] += outR * mVoiceGain;
            if (++w >= n) w = 0;
        }
        mWrite = w;
    }

    bool isPrepared() const noexcept { return mPrepared; }

private:
    std::vector<float> mLine;
    std::size_t mWrite = 0;
    float mRate = 0.0f, mCenter = 0.0f, mHalfDepth = 0.0f, mPhaseInc = 0.0f, mVoiceGain = 0.0f;
    float mPhase[kVoices] = {};
    float mDelayStart[kVoices] = {}, mDelayStep[kVoices] = {};
    bool mPrepared = false;
};

}  // namespace wma
