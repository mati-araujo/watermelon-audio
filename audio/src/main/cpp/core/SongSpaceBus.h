#pragma once

/**
 * @file SongSpaceBus.h
 * @brief Implementacion del bus «Espacio» de la cancion (REQ-055 S12, ADR-0013 §1): ReverbEffect
 *        100 % wet en paralelo con DelayEffect 100 % wet (tiempo en ms, sync apagado), cada uno
 *        con su retorno. Header-only: lo incluye solo AudioEngine.cpp, que ya enlaza effects/.
 *
 * Se crea en el hilo de control en la primera llamada de mezcla y NO se libera mientras viva el
 * motor. Los setters de los efectos no son atomicos entre si: los corre el hilo que procesa
 * (`applySpaceParams` lo invoca el looper en el bloque en que ve una version nueva).
 */

#include "../effects/DelayEffect.h"
#include "../effects/ReverbEffect.h"
#include "../looper/SongSpaceBus.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace wm {

class SongSpaceBusImpl final : public ISongSpaceBus {
public:
    void prepareSpaceBus(int sampleRate, int maxBlockFrames) override {
        mReverb.setSampleRate(sampleRate);
        mDelay.setSampleRate(sampleRate);
        mReverb.setMix(1.0f);
        mDelay.setWet(1.0f);
        mDelay.setSync(false);
        reserveSpaceBus(maxBlockFrames);
        mFresh = true;
        resetSpaceBus();
    }

    void reserveSpaceBus(int maxBlockFrames) override {
        const size_t needed = static_cast<size_t>(std::max(maxBlockFrames, 1)) * 2;
        if (mDelayOut.size() < needed) mDelayOut.resize(needed, 0.0f);
        mCapacityFrames.store(static_cast<int>(mDelayOut.size() / 2), std::memory_order_release);
    }

    int spaceBusCapacityFrames() const override {
        return mCapacityFrames.load(std::memory_order_acquire);
    }

    void applySpaceParams(const SongSpaceParams& p) override {
        mReverb.setDecay(p.reverbDecayS);
        mReverb.setSize(p.reverbSize);
        mReverb.setDamping(p.reverbDamping);
        mReverb.setPreDelay(p.reverbPreDelayMs);
        mReverb.setLowCut(p.reverbLowCutHz);
        mReverb.setHighCut(p.reverbHighCutHz);
        mDelay.setDelayTime(p.delayTimeMs);
        mDelay.setFeedback(p.delayFeedback);
        mReverbReturn = p.reverbReturn;
        mDelayReturn = p.delayReturn;
        // El primer juego de parametros de una instancia fresca fija el tiempo del delay de una:
        // sin esto el suavizado de 50 ms lo desliza desde el default (250 ms) y dos exports
        // coinciden pero ninguno suena al tiempo pedido en sus primeros ecos. Solo el suavizado:
        // las lineas ya las vacio prepareSpaceBus en control (reset() son ~1,5 MB de memset).
        if (mFresh) {
            mDelay.snapDelayTimeToTarget();
            mFresh = false;
        }
    }

    void runSpaceBus(const float* send, float* fxOut, int frames) override {
        const size_t n = static_cast<size_t>(frames) * 2;
        // Los efectos reciben `float*` no const: el bus no escribe sobre el envio.
        mReverb.process(const_cast<float*>(send), fxOut, frames);
        mDelay.process(const_cast<float*>(send), mDelayOut.data(), frames);
        float peak = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const float wetReverb = fxOut[i], wetDelay = mDelayOut[i];
            peak = std::max(peak, std::max(std::abs(wetReverb), std::abs(wetDelay)));
            fxOut[i] = wetReverb * mReverbReturn + wetDelay * mDelayReturn;
        }
        mPreReturnPeak = peak;
    }

    float spaceBusPreReturnPeak() const override { return mPreReturnPeak; }

    void resetSpaceBus() override {
        mReverb.reset();
        mDelay.reset();
        mPreReturnPeak = 0.0f;
        mFresh = true;
    }

private:
    ReverbEffect mReverb;
    DelayEffect mDelay;
    std::vector<float> mDelayOut;
    std::atomic<int> mCapacityFrames{0};
    float mReverbReturn = 0.0f;
    float mDelayReturn = 0.0f;
    float mPreReturnPeak = 0.0f;
    bool mFresh = true;
};

}  // namespace wm
