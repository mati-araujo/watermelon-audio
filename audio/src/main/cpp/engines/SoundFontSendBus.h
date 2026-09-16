/**
 * SoundFontSendBus.h — REQ-040 S3: los dos buses de sends, sus unidades y la compuerta, en UN
 * lugar que producción y el arnés de conformidad cruzan por igual.
 *
 * En S2 esto vivía adentro de `SoundFontEngine` (`mixSendBusesInto` / `clearSendEffects`). Se
 * extrae porque el arnés del spec-test (`MidiSpecHarness.h`) maneja `tsf` como un reproductor
 * MIDI, sin pasar por la fachada táctil, y para juzgar #17/#18 contra la referencia con efectos
 * tiene que mezclar los sends EXACTAMENTE como producción — no con una copia en el test, que
 * mediría "el mecanismo funciona si alguien lo llama" (REQ-012). Misma razón por la que
 * `channelNoteOnWithModulators` es una función libre.
 *
 * Lo que hace, por bloque (decisiones 3, 4 y 5 de la amplificación del 2026-09-15):
 *  - escala los dos buses por la perilla de la ambiencia (`setSendScale`, 0..1 por bus, default 1;
 *    publica desde REQ-042 como `wma_sf_set_ambience`), con un slew de 5 ms en caliente;
 *  - la COMPUERTA, en segundos: con la entrada de los dos buses más de `kTailSeconds` por
 *    debajo de `kSilence` (−90 dBFS) las unidades se saltean y se limpian, y lo que sale es
 *    cero exacto; el primer bloque con send > 0 las reengancha ahí mismo;
 *  - suma el wet del freeverb y del chorus a la salida estéreo.
 *
 * RT: `mixSendBusesInto` y `clearSendEffects` son aritmética sobre buffers que `prepare()`
 * alocó. Nombres únicos (el walker de RT no cubre lo que resuelve a dos definiciones).
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "../platform/Logger.h"
#include "SoundFontChorus.h"
#include "SoundFontReverb.h"

namespace wma {

class SoundFontSendBus {
public:
    static constexpr float kSilence = 3.1623e-5f;   // -90 dBFS
    static constexpr float kTailSeconds = 2.0f;     // > RT60 del freeverb con room 0,2 (~0,75 s)
    /// REQ-042 (AC-042.7): la perilla conmuta EN CALIENTE con un slew de tiempo fijo, en
    /// segundos y no en bloques (el bloque va de 128 a 2048). Medido sin rampa, conmutar
    /// 0/0 → 1/1 con una nota sostenida era hasta 10× mas brusco que el note-on del font.
    static constexpr float kSlewSeconds = 0.005f;

    /**
     * Thread de CONTROL: aloca las dos unidades para este rate y bloques de hasta `maxBlock`.
     * NO toca la perilla (AC-042.4). Deja el slew SIN cebar: el primer bloque que se renderice
     * toma el objetivo de una, sin rampa — las unidades acaban de nacer vacias y no hay nada
     * sonando que pueda hacer click. Es lo que hace que un set ANTES de arrancar aplique de
     * una (0/0 ≡ seco desde la primera muestra) y que solo el cambio en caliente rampee.
     */
    void prepare(float sampleRate, int maxBlock) {
        mSampleRate = sampleRate > 0.0f ? sampleRate : 48000.0f;
        mReverb.prepare(mSampleRate, maxBlock);
        mChorus.prepare(mSampleRate);
        mQuietSeconds = 0.0f;
        mActive = false;
        mSlewStep = 1.0f / (kSlewSeconds * mSampleRate);
        mSlewPrimed = false;
    }

    /**
     * Cualquier thread: la perilla de la ambiencia (REQ-042 sobre la costura de REQ-040), 0..1 por
     * bus, lineal sobre la amplitud del send. El render la lee por bloque.
     *
     * Fuera de rango satura; **NaN deja ese bus como estaba y avisa** (AC-042.2). No es defensa
     * gratuita: `std::clamp(NaN, 0, 1)` devuelve NaN, y un NaN en el atomico multiplicaria el
     * bus entero en el thread de audio. Se decide POR BUS: el valor valido del otro entra igual.
     *
     * NADA la resetea —ni `prepare()`, ni `clearSendEffects()`, ni el cambio de font—: es un
     * ajuste del instrumento, no del font (AC-042.4). El unico escritor es este metodo.
     *
     * Lo que se guarda es el OBJETIVO. En caliente el render lo alcanza con un slew de
     * `kSlewSeconds` por muestra (AC-042.7); los getters devuelven el objetivo, no el valor en
     * transito.
     *
     * NO RT-safe por el aviso (`wma::logMessage`): la llama el thread de control.
     */
    void setSendScale(float reverb, float chorus) noexcept {
        if (std::isnan(reverb)) {
            wma::logMessage(wma::LogLevel::WARN, "SF.Ambience", "ambiencia: reverb NaN ignorado (queda %.3f)",
                            mScaleReverb.load(std::memory_order_relaxed));
        } else {
            mScaleReverb.store(std::clamp(reverb, 0.0f, 1.0f), std::memory_order_relaxed);
        }
        if (std::isnan(chorus)) {
            wma::logMessage(wma::LogLevel::WARN, "SF.Ambience", "ambiencia: chorus NaN ignorado (queda %.3f)",
                            mScaleChorus.load(std::memory_order_relaxed));
        } else {
            mScaleChorus.store(std::clamp(chorus, 0.0f, 1.0f), std::memory_order_relaxed);
        }
    }

    /// Cualquier thread: el OBJETIVO del bus de reverb (lo que dejo `setSendScale`), no el valor en
    /// transito del slew.
    float sendScaleReverb() const noexcept { return mScaleReverb.load(std::memory_order_relaxed); }

    /// Cualquier thread: idem para el bus de chorus.
    float sendScaleChorus() const noexcept { return mScaleChorus.load(std::memory_order_relaxed); }

    /**
     * Thread de AUDIO. `reverbBus` / `chorusBus` son los buses MONO que `tsf_render_float_sends`
     * llenó para este bloque (se escalan en el lugar); `stereoOut` es la salida entrelazada a la
     * que se SUMA el wet. `frames` no puede superar el `maxBlock` de `prepare()`.
     */
    void mixSendBusesInto(float* stereoOut, float* reverbBus, float* chorusBus, int32_t frames) noexcept {
        const float targetR = mScaleReverb.load(std::memory_order_relaxed);
        const float targetC = mScaleChorus.load(std::memory_order_relaxed);
        // El slew (AC-042.7): dos floats de estado que avanzan POR MUESTRA hacia el objetivo con
        // paso 1/(kSlewSeconds·rate). Con el objetivo ya alcanzado el multiplicador es EXACTAMENTE
        // el objetivo (1/1 sigue siendo byte a byte v2.18.0). Sin cebar, toma el objetivo de una.
        if (!mSlewPrimed) {
            mCurrentReverb = targetR;
            mCurrentChorus = targetC;
            mSlewPrimed = true;
        }
        float curR = mCurrentReverb, curC = mCurrentChorus;
        const float step = mSlewStep;
        float peak = 0.0f;
        for (int32_t i = 0; i < frames; ++i) {
            if (curR < targetR) { curR += step; if (curR > targetR) curR = targetR; }
            else if (curR > targetR) { curR -= step; if (curR < targetR) curR = targetR; }
            if (curC < targetC) { curC += step; if (curC > targetC) curC = targetC; }
            else if (curC > targetC) { curC -= step; if (curC < targetC) curC = targetC; }
            reverbBus[i] *= curR;
            chorusBus[i] *= curC;
            const float a = reverbBus[i] < 0.0f ? -reverbBus[i] : reverbBus[i];
            const float b = chorusBus[i] < 0.0f ? -chorusBus[i] : chorusBus[i];
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        mCurrentReverb = curR;
        mCurrentChorus = curC;
        if (peak > kSilence) {
            mQuietSeconds = 0.0f;
            mActive = true;
        } else if (mActive) {
            mQuietSeconds += static_cast<float>(frames) / mSampleRate;
            if (mQuietSeconds > kTailSeconds) clearSendEffects();
        }
        if (!mActive) return;
        mReverb.addReverbWetFromMono(reverbBus, stereoOut, frames);
        mChorus.addChorusWetFromMono(chorusBus, stereoOut, frames);
    }

    /// Thread de AUDIO. Vacía las dos unidades y apaga la compuerta: el próximo bloque es cero.
    void clearSendEffects() noexcept {
        mReverb.clearReverbTail();
        mChorus.clearChorusTail();
        mQuietSeconds = 0.0f;
        mActive = false;
    }

    /// Audio-thread only: si hay cola viva (la compuerta abierta).
    bool isActive() const noexcept { return mActive; }

private:
    SoundFontReverb mReverb;
    SoundFontChorus mChorus;
    std::atomic<float> mScaleReverb{1.0f};
    std::atomic<float> mScaleChorus{1.0f};
    float mSampleRate = 48000.0f;
    float mQuietSeconds = 0.0f;   // audio-thread only
    bool mActive = false;         // audio-thread only
    // El slew de AC-042.7 (audio-thread only, salvo `prepare()` con el audio parado).
    float mCurrentReverb = 1.0f;
    float mCurrentChorus = 1.0f;
    float mSlewStep = 1.0f / (kSlewSeconds * 48000.0f);
    bool mSlewPrimed = false;
};

}  // namespace wma
