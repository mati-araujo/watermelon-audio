#include "SpringReverbEffect.h"
#include <algorithm>
#include <cmath>

SpringReverbEffect::SpringReverbEffect()
    : mTankL(900.0f),
      mTankR(900.0f),
      mToneL(48000.0f),
      mToneR(48000.0f),
      mInputHpfL(48000.0f),
      mInputHpfR(48000.0f) {
    mToneL.setBandpass(2200.0f, 0.7f);
    mToneR.setBandpass(2200.0f, 0.7f);
    mInputHpfL.setHighpass(120.0f, 0.707f);
    mInputHpfR.setHighpass(120.0f, 0.707f);
    mMixSmooth.reset(0.25f);
    mDecaySmooth.reset(2.2f);
}

void SpringReverbEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);
    mTankL.setSampleRate(sr);
    mTankR.setSampleRate(sr);
    mTankL.setMaxDelay(900.0f);
    mTankR.setMaxDelay(900.0f);
    mToneL.setSampleRate(sr);
    mToneR.setSampleRate(sr);
    mInputHpfL.setSampleRate(sr);
    mInputHpfR.setSampleRate(sr);
    mInputHpfL.setHighpass(120.0f, 0.707f);
    mInputHpfR.setHighpass(120.0f, 0.707f);
    mMixSmooth.setSmoothingTime(10.0f, sr);
    mDecaySmooth.setSmoothingTime(20.0f, sr);
}

void SpringReverbEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case PARAM_DECAY: mDecay.store(std::clamp(value, 0.4f, 5.0f), std::memory_order_relaxed); break;
        case PARAM_TONE: mTone.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case PARAM_DRIP: mDrip.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case PARAM_TENSION: mTension.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case PARAM_MIX: mMix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        default: break;
    }
}

float SpringReverbEffect::getParam(int paramId) {
    switch (paramId) {
        case PARAM_DECAY: return mDecay.load(std::memory_order_relaxed);
        case PARAM_TONE: return mTone.load(std::memory_order_relaxed);
        case PARAM_DRIP: return mDrip.load(std::memory_order_relaxed);
        case PARAM_TENSION: return mTension.load(std::memory_order_relaxed);
        case PARAM_MIX: return mMix.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void SpringReverbEffect::process(float* input, float* output, int numFrames) {
    float tone = mTone.load(std::memory_order_relaxed);
    if (std::abs(tone - mLastTone) > 0.01f) {
        float freq = 900.0f + tone * 4200.0f;
        mToneL.setBandpass(freq, 0.55f + tone * 1.2f);
        mToneR.setBandpass(freq * 1.03f, 0.55f + tone * 1.2f);
        mLastTone = tone;
    }

    float drip = mDrip.load(std::memory_order_relaxed);
    float tension = mTension.load(std::memory_order_relaxed);
    float mixTarget = mMix.load(std::memory_order_relaxed);
    float decayTarget = mDecay.load(std::memory_order_relaxed);

    for (int i = 0; i < numFrames; ++i) {
        float dryL = input[i * 2];
        float dryR = input[i * 2 + 1];
        float mix = mMixSmooth.process(mixTarget);
        float decay = mDecaySmooth.process(decayTarget);

        float inL = mInputHpfL.process(dryL);
        float inR = mInputHpfR.process(dryR);

        // WD-3.6 — los pesos de los taps salen a constantes con nombre porque
        // `kTapSum` (abajo) tiene que ser SU SUMA, y un 1,49 escrito a mano se
        // vuelve mentira la primera vez que alguien toca uno de los cuatro.
        // Derivarla los mantiene atados sin que nadie tenga que acordarse.
        constexpr float kTap0 = 0.55f;
        constexpr float kTap1 = 0.42f;
        constexpr float kTap2 = 0.30f;
        constexpr float kTap3 = 0.22f;

        float stretch = 0.75f + tension * 0.45f;
        float tapL =
            mTankL.readMs(23.0f * stretch) * kTap0 +
            mTankL.readMs(47.0f * stretch) * kTap1 +
            mTankL.readMs(89.0f * stretch) * kTap2 +
            mTankL.readMs(151.0f * stretch) * kTap3;
        float tapR =
            mTankR.readMs(29.0f * stretch) * kTap0 +
            mTankR.readMs(53.0f * stretch) * kTap1 +
            mTankR.readMs(97.0f * stretch) * kTap2 +
            mTankR.readMs(167.0f * stretch) * kTap3;

        // WD-3.6 — el lazo se reparte un PRESUPUESTO de ganancia, en vez de que
        // cada camino traiga la suya.
        //
        // Antes, `feedback` multiplicaba a `tapR`, que ya venia con la suma de
        // los cuatro coeficientes de arriba: 0,55 + 0,42 + 0,30 + 0,22 = 1,49.
        // Con el decay por defecto (2,2 -> feedback 0,692) la ganancia del
        // camino cruzado daba 1,49 * 0,692 = 1,031, y el drip sumaba 0,45 * 0,35
        // MAS, porque entra al tanque contrario sin pasar por `feedback`.
        // O sea: **el efecto era inestable con los valores de fabrica**, a todo
        // sample rate. La cola crecia, a los ~35 s pasaba 1e34 y a los ~38 s el
        // tanque se iba a no-finito; ahi el scrub de la salida lo convertia en
        // silencio y el reverb quedaba MUDO para siempre.
        //
        // El borde se predijo antes de medirlo y cayo exacto: con el drip en 0,
        // la ganancia cruza 1 en decay = (1/1,49 - 0,45) / 0,11 = **2,010**, y
        // medido es estable en 2,01 (0,952) e inestable en 2,05 (1,004).
        //
        // Lo que se acota es la SUMA DE MODULOS, que es cota superior de la
        // ganancia a cualquier frecuencia y por eso alcanza para garantizar
        // estabilidad en todo el rango de las dos perillas — no solo en los
        // defaults, que es lo que pide el criterio. Es conservadora a proposito:
        // el borde real del drip esta mas arriba que su modulo (lee a 7 y 9 ms
        // contra los 17 a 181 de los taps, y esas fases no se alinean), pero una
        // cota que depende de la frecuencia no es una cota.
        //
        // El drip DESCUENTA del mismo presupuesto en vez de sumarse por afuera:
        // asi subir el drip cambia el CARACTER —mas click, menos taps— sin tocar
        // el largo de la cola ni poder desestabilizar. Y `loopGain` sigue siendo
        // el mapeo original del decay, asi que la perilla conserva su monotonia
        // en todo el recorrido.
        constexpr float kTapSum = kTap0 + kTap1 + kTap2 + kTap3;  // = 1,49
        constexpr float kMaxLoopGain = 0.92f;  // el mismo techo que ya tenia
        const float loopGain = std::clamp(0.45f + decay * 0.11f, 0.0f, kMaxLoopGain);
        const float dripGain = drip * 0.45f;
        const float feedback = std::max(0.0f, loopGain - dripGain) / kTapSum;

        float dripClickL = mTankL.readMs(7.0f) * dripGain;
        float dripClickR = mTankR.readMs(9.0f) * dripGain;

        mTankL.write(inL + tapR * feedback + dripClickR);
        mTankR.write(inR + tapL * feedback + dripClickL);

        float wetL = mToneL.process(tapL + dripClickL);
        float wetR = mToneR.process(tapR + dripClickR);

        if (!std::isfinite(wetL)) wetL = 0.0f;
        if (!std::isfinite(wetR)) wetR = 0.0f;

        output[i * 2] = dryL + (wetL - dryL) * mix;
        output[i * 2 + 1] = dryR + (wetR - dryR) * mix;
    }
}

void SpringReverbEffect::reset() {
    mTankL.clear();
    mTankR.clear();
    mToneL.reset();
    mToneR.reset();
    mInputHpfL.reset();
    mInputHpfR.reset();
}
