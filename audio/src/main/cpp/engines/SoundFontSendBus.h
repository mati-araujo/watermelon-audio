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
 *    publica desde REQ-042 como `wma_sf_set_ambience`);
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

    /// Thread de CONTROL: aloca las dos unidades para este rate y bloques de hasta `maxBlock`.
    void prepare(float sampleRate, int maxBlock) {
        mSampleRate = sampleRate > 0.0f ? sampleRate : 48000.0f;
        mReverb.prepare(mSampleRate, maxBlock);
        mChorus.prepare(mSampleRate);
        mQuietSeconds = 0.0f;
        mActive = false;
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

    /// Cualquier thread: lo que el render va a aplicar sobre el bus de reverb en el proximo bloque.
    float sendScaleReverb() const noexcept { return mScaleReverb.load(std::memory_order_relaxed); }

    /// Cualquier thread: idem para el bus de chorus.
    float sendScaleChorus() const noexcept { return mScaleChorus.load(std::memory_order_relaxed); }

    /**
     * Thread de AUDIO. `reverbBus` / `chorusBus` son los buses MONO que `tsf_render_float_sends`
     * llenó para este bloque (se escalan en el lugar); `stereoOut` es la salida entrelazada a la
     * que se SUMA el wet. `frames` no puede superar el `maxBlock` de `prepare()`.
     */
    void mixSendBusesInto(float* stereoOut, float* reverbBus, float* chorusBus, int32_t frames) noexcept {
        const float scaleR = mScaleReverb.load(std::memory_order_relaxed);
        const float scaleC = mScaleChorus.load(std::memory_order_relaxed);
        float peak = 0.0f;
        for (int32_t i = 0; i < frames; ++i) {
            reverbBus[i] *= scaleR;
            chorusBus[i] *= scaleC;
            const float a = reverbBus[i] < 0.0f ? -reverbBus[i] : reverbBus[i];
            const float b = chorusBus[i] < 0.0f ? -chorusBus[i] : chorusBus[i];
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
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
};

}  // namespace wma
