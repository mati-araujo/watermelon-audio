#pragma once
#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include "../dsp/BiquadFilter.h"
#include <atomic>
#include <cmath>
#include <algorithm>

/**
 * @class DeciHpfEffect
 * @brief Decimator with HPF in series (KORG NTS-3 FX-005)
 *
 * Combines a high-pass filter with bit crushing and sample rate reduction.
 * Signal flows: Input → HPF → Bit Crush → SR Reduce → Mix → Output
 * The HPF removes low frequencies before decimation, producing a more
 * defined lo-fi sound focused on mids and highs.
 */
class DeciHpfEffect : public Effect {
public:
    DeciHpfEffect();
    ~DeciHpfEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    /// Limpia el HPF, el hold y re-siembra los smoothers (WD-3.2).
    void reset() override;

    /**
     * @brief Latencia = los samples que el zero-order hold retiene (WD-3.1).
     *
     * La reduccion de sample rate es un sample-and-hold: `mHoldCounter` arranca
     * en cero y el valor retenido no se actualiza hasta alcanzar `step`, asi que
     * los primeros `step` samples salen con el hold inicial. Ese corrimiento ES
     * latencia del camino directo.
     *
     * **Depende del parametro**, y por eso se calcula en vez de declararse una
     * constante: `step = sampleRate / targetSR`. Es el caso que el contrato de
     * Effect::getLatencySamples() nombra — un efecto cuyo retardo cambia con sus
     * parametros devuelve el valor vigente.
     */
    int getLatencySamples() const override;

    // Parameter IDs
    static constexpr int PARAM_BIT_DEPTH = 0;     // 1-24 bits (XY: X)
    static constexpr int PARAM_HPF_CUTOFF = 1;    // 20-8000 Hz (XY: Y)
    static constexpr int PARAM_SAMPLE_RATE = 2;   // 100-48000 Hz (XY: Depth)
    static constexpr int PARAM_MIX = 3;           // 0-1

private:
    // HPF filters (stereo)
    BiquadFilter mHpfL;
    BiquadFilter mHpfR;

    // Decimator state (inline, no separate effect instance)
    float mHoldL = 0.0f;
    float mHoldR = 0.0f;
    float mHoldCounter = 0.0f;

    // Parameters
    std::atomic<float> mBitDepth{12.0f};
    std::atomic<float> mHpfCutoff{300.0f};
    std::atomic<float> mTargetSR{12000.0f};
    std::atomic<float> mMix{1.0f};

    // Smoothers
    ParameterSmoother mBitDepthSmooth;
    ParameterSmoother mCutoffSmooth;
    ParameterSmoother mSRSmooth;
    ParameterSmoother mMixSmooth;

    int mSampleRate = 48000;
    float mLastCutoff = 0.0f;  // Track cutoff for filter recalculation
};
