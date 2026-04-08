#pragma once

#include "SynthEngine.h"
#include <cmath>
#include <array>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @class SupersawEngine
 * @brief Supersaw unison synthesis engine
 *
 * Multiple detuned sawtooth oscillators stacked for thick, fat sound.
 * Classic trance/EDM pad and lead sound (Roland JP-8000 style).
 *
 * Algorithm:
 *   For each voice i of N:
 *     detune_factor = 1.0 + detune_cents × (i/(N-1) - 0.5) / 1200
 *     phase[i] += frequency × detune_factor / sampleRate
 *     saw[i] = 2 × phase[i] - 1  (naive saw, PolyBLEP applied)
 *     pan[i] = spread × (i/(N-1) - 0.5)  (stereo distribution)
 *   output = sum(saw[i] × pan_gain[i]) / sqrt(N)
 *
 * Parameters:
 *   0 - Detune: detuning amount (0=unison, 1=±50 cents)
 *   1 - Voices: number of voices (0→2, 0.5→5, 1→8)
 *   2 - Spread: stereo distribution (0=mono, 1=full stereo)
 *
 * RT-Safety: Fixed array of MAX_SAW_VOICES, no allocations.
 */
class SupersawEngine : public SynthEngine {
public:
    static constexpr int PARAM_DETUNE = 0;
    static constexpr int PARAM_VOICES = 1;
    static constexpr int PARAM_SPREAD = 2;

    static constexpr int MAX_SAW_VOICES = 8;

    SupersawEngine() {
        mParams[PARAM_DETUNE].store(0.3f, std::memory_order_relaxed);
        mParams[PARAM_VOICES].store(0.4f, std::memory_order_relaxed);
        mParams[PARAM_SPREAD].store(0.5f, std::memory_order_relaxed);

        mPhases.fill(0.0f);
    }

    void prepare(int32_t sampleRate, int32_t maxBlockSize) override {
        SynthEngine::prepare(sampleRate, maxBlockSize);
        mInvSampleRate = 1.0f / static_cast<float>(sampleRate);
        // Randomize initial phases for natural chorus effect
        uint32_t rng = 67890;
        for (int i = 0; i < MAX_SAW_VOICES; ++i) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            mPhases[i] = static_cast<float>(rng & 0xFFFF) / 65536.0f;
        }
    }

    void reset() override {
        // Don't reset phases — keep the random distribution for natural sound
        // Only reset on prepare()
    }

    void process(float* buffer, int32_t numFrames,
                 float frequency, float amplitude) override {
        // Read parameters with smoothing (prevents zipper noise)
        const float detuneNorm = smoothParam(PARAM_DETUNE);
        const float voicesNorm = smoothParam(PARAM_VOICES);
        const float spreadNorm = smoothParam(PARAM_SPREAD);

        // Map params
        // Detune: 0-1 → 0-50 cents
        const float detuneCents = detuneNorm * 50.0f;

        // Voices: 0-1 → 2-8 (quantized to integers)
        const int numVoices = std::clamp(
            static_cast<int>(std::round(2.0f + voicesNorm * 6.0f)), 2, MAX_SAW_VOICES);

        // Spread: 0-1
        const float spread = spreadNorm;

        // Normalization factor to prevent volume increase with more voices
        const float normGain = 1.0f / std::sqrt(static_cast<float>(numVoices));

        // Pre-calculate per-voice parameters
        struct VoiceParams {
            float freqMult;   // Frequency multiplier (detune)
            float panL;       // Left gain
            float panR;       // Right gain
        };

        std::array<VoiceParams, MAX_SAW_VOICES> voices{};
        for (int v = 0; v < numVoices; ++v) {
            // Detune: symmetric spread around center
            float detunePosition = (numVoices > 1)
                ? (static_cast<float>(v) / static_cast<float>(numVoices - 1) - 0.5f)
                : 0.0f;
            float centOffset = detuneCents * detunePosition;
            voices[v].freqMult = std::pow(2.0f, centOffset / 1200.0f);

            // Equal-power pan
            float panPosition = 0.5f + spread * detunePosition; // 0-1 range
            panPosition = std::clamp(panPosition, 0.0f, 1.0f);
            voices[v].panL = std::cos(panPosition * static_cast<float>(M_PI) * 0.5f);
            voices[v].panR = std::sin(panPosition * static_cast<float>(M_PI) * 0.5f);
        }

        const float baseInc = frequency * mInvSampleRate;

        for (int32_t i = 0; i < numFrames; ++i) {
            float sumL = 0.0f;
            float sumR = 0.0f;

            for (int v = 0; v < numVoices; ++v) {
                // Advance phase
                float inc = baseInc * voices[v].freqMult;
                mPhases[v] += inc;
                if (mPhases[v] >= 1.0f) mPhases[v] -= 1.0f;

                // Naive sawtooth: 2*phase - 1
                float saw = 2.0f * mPhases[v] - 1.0f;

                // PolyBLEP correction for anti-aliasing
                float t = mPhases[v];
                float dt = inc;
                if (t < dt) {
                    t /= dt;
                    saw -= (t + t - t * t - 1.0f);
                } else if (t > (1.0f - dt)) {
                    t = (t - 1.0f) / dt;
                    saw -= (t * t + t + t + 1.0f);
                }

                sumL += saw * voices[v].panL;
                sumR += saw * voices[v].panR;
            }

            // Apply normalization, amplitude
            float outL = sumL * normGain * amplitude;
            float outR = sumR * normGain * amplitude;

            // Sanitize
            if (!std::isfinite(outL)) outL = 0.0f;
            if (!std::isfinite(outR)) outR = 0.0f;

            buffer[i * 2]     = outL;
            buffer[i * 2 + 1] = outR;
        }
    }

    const char* getName() const override { return "Supersaw"; }
    int getParameterCount() const override { return 3; }

    EngineParameterDef getParameterDef(int paramId) const override {
        switch (paramId) {
            case PARAM_DETUNE:
                return {"Detune", "DETUNE", 0.0f, 1.0f, 0.3f};
            case PARAM_VOICES:
                return {"Voices", "VOICES", 0.0f, 1.0f, 0.4f};
            case PARAM_SPREAD:
                return {"Spread", "SPREAD", 0.0f, 1.0f, 0.5f};
            default:
                return {"Unknown", "?", 0.0f, 1.0f, 0.0f};
        }
    }

private:
    std::array<float, MAX_SAW_VOICES> mPhases{};
    float mInvSampleRate = 1.0f / 48000.0f;
};
