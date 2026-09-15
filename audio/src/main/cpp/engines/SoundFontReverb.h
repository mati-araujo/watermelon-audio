/**
 * SoundFontReverb.h — REQ-040 S2: la reverb FIJA del SoundFont.
 *
 * ES EL SONIDO DEL FONT, NO UN EFECTO DEL USUARIO. Un SoundFont declara por zona cuánto de cada
 * voz manda a una reverb (`reverbEffectsSend`, 0..1) y espera que el sintetizador tenga UNA; el
 * spec (SF2 §8.1.2 #16) no dice cuál ni con qué parámetros. La única referencia con números es
 * FluidSynth 2.6.0: sus cuatro parámetros por default (room 0,2 · damp 0 · width 0,5 · level 0,9)
 * son los que juzgan las pruebas #17 del SoundFont-Spec-Test contra su render `-R 1`.
 *
 * EL ALGORITMO ES FREEVERB CLÁSICO (Jezar at Dreampoint, dominio público): 8 combs con lowpass
 * en el feedback + 4 allpass en serie por canal, el canal derecho corrido 23 muestras
 * (`stereospread`). El de FluidSynth 2.6.0 NO es este —desde 2.1 es una red FDN con líneas
 * moduladas, LGPL— así que lo que se toma de FluidSynth es el MAPEO de parámetros (los mismos
 * `scaleroom`/`offsetroom`/`scalewet`/`scaledamp` de Freeverb, que FluidSynth conservó) y no la
 * estructura. Decidido el 2026-09-15 (amplificación, decisión 2, con la salvedad ratificada): la
 * clase de #17 contra la referencia sale de la MEDICIÓN en S3, no de esta decisión.
 *
 * El diseño de Freeverb tiene entrada MONO (lo que el bus lleva: `send × voz`, sin paneo) y
 * salida estéreo por `wet1`/`wet2` (el `width`). Los largos de los combs y allpass están
 * afinados a 44 100 Hz y se escalan por la tasa de salida, como hace FluidSynth.
 *
 * RT: `addReverbWetFromMono` es aritmética sobre buffers que `prepare()` alocó en el thread de control.
 * Sin alocar, sin lock, sin log. Los nombres son ÚNICOS a propósito (no `process`/`reset`): un
 * método con nombre común en cualquier parte del árbol le saca cobertura al lint de RT.
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wma {

class SoundFontReverb {
public:
    // Los parámetros por default de FluidSynth 2.6.0 (`synth.reverb.*`), que son los de la
    // referencia `-R 1` del spec-test. Fijos: es el sonido del font.
    static constexpr float kRoomSize = 0.2f;
    static constexpr float kDamp = 0.0f;
    static constexpr float kWidth = 0.5f;
    static constexpr float kLevel = 0.9f;

    /// Thread de CONTROL: aloca. Dimensiona las líneas para `sampleRate` y el scratch para
    /// bloques de hasta `maxBlock` frames (un bloque más grande cae al camino por muestra).
    void prepare(float sampleRate, int maxBlock = 2048) {
        const float scale = sampleRate / 44100.0f;
        for (int c = 0; c < kCombs; ++c) {
            mCombL[c].combResize(static_cast<std::size_t>(std::lround(kCombTuning[c] * scale)));
            mCombR[c].combResize(static_cast<std::size_t>(std::lround((kCombTuning[c] + kStereoSpread) * scale)));
        }
        for (int a = 0; a < kAllpasses; ++a) {
            mAllpassL[a].allpassResize(static_cast<std::size_t>(std::lround(kAllpassTuning[a] * scale)));
            mAllpassR[a].allpassResize(static_cast<std::size_t>(std::lround((kAllpassTuning[a] + kStereoSpread) * scale)));
        }
        mScratchIn.assign(static_cast<std::size_t>(maxBlock > 0 ? maxBlock : 1), 0.0f);
        mScratchL.assign(mScratchIn.size(), 0.0f);
        mScratchR.assign(mScratchIn.size(), 0.0f);
        // El mapeo de Freeverb, el que FluidSynth conservó: room -> feedback de los combs,
        // damp -> lowpass del feedback, level -> wet, width -> reparto L/R.
        mFeedback = kRoomSize * kScaleRoom + kOffsetRoom;   // 0,756
        mDamp1 = kDamp * kScaleDamp;                         // 0
        mDamp2 = 1.0f - mDamp1;
        const float wet = kLevel * kScaleWet;                // 2,7
        mWet1 = wet * (kWidth * 0.5f + 0.5f);                // 2,025
        mWet2 = wet * ((1.0f - kWidth) * 0.5f);              // 0,675
        clearReverbTail();
        mPrepared = true;
    }

    /// RT. Vacía las líneas: el bloque siguiente no lleva nada de lo anterior.
    void clearReverbTail() noexcept {
        for (auto& c : mCombL) c.combClear();
        for (auto& c : mCombR) c.combClear();
        for (auto& a : mAllpassL) a.allpassClear();
        for (auto& a : mAllpassR) a.allpassClear();
    }

    /**
     * RT. Suma a `stereoOut` (entrelazado, `frames` pares L/R) el wet de `monoIn` (`frames`
     * muestras, el bus de sends ya escalado). No toca el dry: el engine ya lo dejó en `stereoOut`.
     *
     * Se recorre UNA línea por vez sobre el bloque entero (y no todas las líneas por muestra):
     * cada comb/allpass es un bucle apretado con punteros crudos, que es lo que hace que el
     * costo medido sea el del DSP y no el de las llamadas — el build de host es -O0.
     */
    void addReverbWetFromMono(const float* monoIn, float* stereoOut, int frames) noexcept {
        if (!mPrepared) return;
        if (static_cast<std::size_t>(frames) > mScratchIn.size()) frames = static_cast<int>(mScratchIn.size());
        float* in = mScratchIn.data();
        float* accL = mScratchL.data();
        float* accR = mScratchR.data();
        for (int i = 0; i < frames; ++i) { in[i] = monoIn[i] * kFixedGain; accL[i] = 0.0f; accR[i] = 0.0f; }
        for (int c = 0; c < kCombs; ++c) {
            mCombL[c].combRun(in, accL, frames, mFeedback, mDamp1, mDamp2);
            mCombR[c].combRun(in, accR, frames, mFeedback, mDamp1, mDamp2);
        }
        for (int a = 0; a < kAllpasses; ++a) {
            mAllpassL[a].allpassRun(accL, frames);
            mAllpassR[a].allpassRun(accR, frames);
        }
        for (int i = 0; i < frames; ++i) {
            stereoOut[2 * i] += accL[i] * mWet1 + accR[i] * mWet2;
            stereoOut[2 * i + 1] += accR[i] * mWet1 + accL[i] * mWet2;
        }
    }

    bool isPrepared() const noexcept { return mPrepared; }

private:
    // Las constantes de Freeverb, tal cual (afinadas a 44,1 kHz).
    static constexpr int kCombs = 8;
    static constexpr int kAllpasses = 4;
    static constexpr int kStereoSpread = 23;
    static constexpr int kCombTuning[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr int kAllpassTuning[kAllpasses] = {556, 441, 341, 225};
    static constexpr float kFixedGain = 0.015f;
    static constexpr float kScaleWet = 3.0f;
    static constexpr float kScaleDamp = 0.4f;
    static constexpr float kScaleRoom = 0.28f;
    static constexpr float kOffsetRoom = 0.7f;
    static constexpr float kAllpassFeedback = 0.5f;

    struct Comb {
        std::vector<float> buf;
        std::size_t idx = 0;
        float filterStore = 0.0f;
        void combResize(std::size_t n) { buf.assign(n > 0 ? n : 1, 0.0f); idx = 0; filterStore = 0.0f; }
        void combClear() noexcept { for (float& x : buf) x = 0.0f; idx = 0; filterStore = 0.0f; }
        /// Suma a `acc` la salida del comb para `in`, `frames` muestras.
        void combRun(const float* in, float* acc, int frames, float feedback, float damp1, float damp2) noexcept {
            float* b = buf.data();
            const std::size_t n = buf.size();
            std::size_t i = idx;
            float fs = filterStore;
            for (int k = 0; k < frames; ++k) {
                const float output = b[i];
                fs = output * damp2 + fs * damp1;
                b[i] = in[k] + fs * feedback;
                if (++i >= n) i = 0;
                acc[k] += output;
            }
            idx = i;
            filterStore = fs;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        std::size_t idx = 0;
        void allpassResize(std::size_t n) { buf.assign(n > 0 ? n : 1, 0.0f); idx = 0; }
        void allpassClear() noexcept { for (float& x : buf) x = 0.0f; idx = 0; }
        /// Reemplaza `x` por su salida allpass, `frames` muestras.
        void allpassRun(float* x, int frames) noexcept {
            float* b = buf.data();
            const std::size_t n = buf.size();
            std::size_t i = idx;
            for (int k = 0; k < frames; ++k) {
                const float input = x[k];
                const float bufout = b[i];
                x[k] = -input + bufout;
                b[i] = input + bufout * kAllpassFeedback;
                if (++i >= n) i = 0;
            }
            idx = i;
        }
    };

    Comb mCombL[kCombs], mCombR[kCombs];
    Allpass mAllpassL[kAllpasses], mAllpassR[kAllpasses];
    std::vector<float> mScratchIn, mScratchL, mScratchR;
    float mFeedback = 0.0f, mDamp1 = 0.0f, mDamp2 = 1.0f, mWet1 = 0.0f, mWet2 = 0.0f;
    bool mPrepared = false;
};

}  // namespace wma
