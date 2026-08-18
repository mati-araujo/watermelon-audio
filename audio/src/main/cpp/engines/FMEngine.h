#pragma once

#include "SynthEngine.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @class FMEngine
 * @brief 2-operator Frequency Modulation synthesis engine
 *
 * Classic FM synthesis: a modulator oscillator modulates the frequency
 * of a carrier oscillator, producing complex harmonic spectra.
 *
 * Algorithm:
 *   modulator_phase += 2π × (carrier_freq × ratio) / sampleRate
 *   mod_out = sin(modulator_phase + feedback × prev_mod_out)
 *   carrier_phase += 2π × carrier_freq / sampleRate
 *   output = sin(carrier_phase + modIndex × mod_out) × amplitude
 *
 * Parameters:
 *   0 - Mod Index: modulation intensity (0=clean sine, 1=rich harmonics)
 *                  Internally mapped to 0-10 for musical range
 *   1 - Ratio: carrier:modulator frequency ratio (quantized to integers 1-8)
 *              XY-friendly: smooth sweep through harmonic ratios
 *   2 - Feedback: modulator self-feedback (0=clean, 1=adds odd harmonics/noise)
 *
 * Sonic character:
 *   - Low index + integer ratio → bells, electric piano, vibraphone
 *   - High index + integer ratio → brass, metallic leads
 *   - High index + non-integer ratio → inharmonic, clangorous
 *   - High feedback → increasingly noisy, distorted
 *
 * RT-Safety: No allocations, all state in fixed members.
 */
class FMEngine : public SynthEngine {
public:
    static constexpr int PARAM_MOD_INDEX = 0;
    static constexpr int PARAM_RATIO = 1;
    static constexpr int PARAM_FEEDBACK = 2;

    FMEngine() {
        mParams[PARAM_MOD_INDEX].store(0.3f, std::memory_order_relaxed);
        mParams[PARAM_RATIO].store(0.25f, std::memory_order_relaxed);
        mParams[PARAM_FEEDBACK].store(0.0f, std::memory_order_relaxed);
    }

    void prepare(int32_t sampleRate, int32_t maxBlockSize) override {
        SynthEngine::prepare(sampleRate, maxBlockSize);
        mCarrierPhase = 0.0f;
        mModulatorPhase = 0.0f;
        mPrevModOut = 0.0f;
        mInvSampleRate = 1.0f / static_cast<float>(sampleRate);
    }

    void reset() override {
        mCarrierPhase = 0.0f;
        mModulatorPhase = 0.0f;
        mPrevModOut = 0.0f;
    }

    void process(float* buffer, int32_t numFrames,
                 float frequency, float amplitude) override {
        // Read parameters with smoothing (prevents zipper noise)
        const float modIndexNorm = smoothParam(PARAM_MOD_INDEX, numFrames);
        const float ratioNorm = smoothParam(PARAM_RATIO, numFrames);
        const float feedbackNorm = smoothParam(PARAM_FEEDBACK, numFrames);

        // Map normalized params to musical ranges
        // Mod index: 0-1 → 0-10 (musical range for 2-op FM)
        const float modIndex = modIndexNorm * 10.0f;

        // Ratio: 0-1 → quantized to 1,2,3,4,5,6,7,8
        // Smooth interpolation between ratios for XY control
        const float ratioFloat = 1.0f + ratioNorm * 7.0f; // 1.0 to 8.0
        // Quantize to nearest 0.5 for semi-harmonic ratios
        const float ratio = std::round(ratioFloat * 2.0f) / 2.0f;

        // Feedback: 0-1 → 0-1.2 (beyond 1.0 gets increasingly chaotic)
        const float feedback = feedbackNorm * 1.2f;

        // Phase increments
        const float carrierInc = frequency * mInvSampleRate;
        const float modulatorInc = frequency * ratio * mInvSampleRate;

        const float twoPi = 2.0f * static_cast<float>(M_PI);

        for (int32_t i = 0; i < numFrames; ++i) {
            // Modulator with self-feedback
            float modPhaseWithFeedback = mModulatorPhase * twoPi + feedback * mPrevModOut;
            float modOut = std::sin(modPhaseWithFeedback);
            mPrevModOut = modOut;

            // Carrier modulated by modulator output
            float carrierPhaseModulated = mCarrierPhase * twoPi + modIndex * modOut;
            float sample = std::sin(carrierPhaseModulated) * amplitude;

            // Advance phases (normalized 0-1, wrapping)
            mCarrierPhase += carrierInc;
            mModulatorPhase += modulatorInc;

            // Wrap phases to prevent float precision loss over time
            if (mCarrierPhase >= 1.0f) mCarrierPhase -= std::floor(mCarrierPhase);
            if (mModulatorPhase >= 1.0f) mModulatorPhase -= std::floor(mModulatorPhase);

            // Sanitize
            if (!std::isfinite(sample)) sample = 0.0f;

            // Stereo output
            buffer[i * 2]     = sample;
            buffer[i * 2 + 1] = sample;
        }
    }

    const char* getName() const override { return "FM Synth"; }
    int getParameterCount() const override { return 3; }

    EngineParameterDef getParameterDef(int paramId) const override {
        switch (paramId) {
            case PARAM_MOD_INDEX:
                return {"Mod Index", "INDEX", 0.0f, 1.0f, 0.3f};
            case PARAM_RATIO:
                return {"Ratio", "RATIO", 0.0f, 1.0f, 0.25f};
            case PARAM_FEEDBACK:
                return {"Feedback", "FDBK", 0.0f, 1.0f, 0.0f};
            default:
                return {"Unknown", "?", 0.0f, 1.0f, 0.0f};
        }
    }

private:
    float mCarrierPhase = 0.0f;      // Normalized phase [0, 1)
    float mModulatorPhase = 0.0f;    // Normalized phase [0, 1)
    float mPrevModOut = 0.0f;        // Previous modulator output (for feedback)
    float mInvSampleRate = 1.0f / 48000.0f;
};
