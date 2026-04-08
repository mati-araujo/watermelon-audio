#pragma once

#include "SynthEngine.h"
#include "../dsp/DelayLine.h"
#include <cmath>
#include <random>

/**
 * @class KarplusStrongEngine
 * @brief Physical modeling synthesis via Karplus-Strong algorithm
 *
 * Produces plucked string, percussion, and kalimba-like sounds.
 *
 * Algorithm:
 *   1. Excitation: short burst of noise/impulse (duration ≈ 1 period)
 *   2. Delay line: circular buffer, length = sampleRate / frequency
 *   3. Lowpass filter in feedback loop: one-pole LP for string damping
 *   4. Feedback coefficient controls decay time
 *   5. Output: delay line output × amplitude
 *
 * Parameters:
 *   0 - Brightness: LP filter cutoff (0=dark warm, 1=bright metallic)
 *   1 - Decay: feedback coefficient (0=short percussive, 1=long sustain)
 *   2 - Excitation: burst character (0=noise, 0.5=impulse, 1=swept)
 *
 * RT-Safety:
 *   - Delay line pre-allocated in prepare() for lowest frequency (20 Hz)
 *   - No allocations in process()
 *   - Random generator uses deterministic xorshift (no syscalls)
 */
class KarplusStrongEngine : public SynthEngine {
public:
    static constexpr int PARAM_BRIGHTNESS = 0;
    static constexpr int PARAM_DECAY = 1;
    static constexpr int PARAM_EXCITATION = 2;

    KarplusStrongEngine() {
        // Set defaults
        mParams[PARAM_BRIGHTNESS].store(0.5f, std::memory_order_relaxed);
        mParams[PARAM_DECAY].store(0.5f, std::memory_order_relaxed);
        mParams[PARAM_EXCITATION].store(0.3f, std::memory_order_relaxed);
    }

    void prepare(int32_t sampleRate, int32_t maxBlockSize) override {
        SynthEngine::prepare(sampleRate, maxBlockSize);

        // Pre-allocate delay line for lowest frequency (20 Hz = 50ms)
        float maxDelayMs = 1000.0f / 20.0f + 1.0f; // ~51ms
        mDelayLine = DelayLine(maxDelayMs, static_cast<float>(sampleRate));

        mFilterStateL = 0.0f;
        mFilterStateR = 0.0f;
        mPrevFrequency = 0.0f;
        mExcitationRemaining = 0;
        mRngState = 12345; // Deterministic seed
    }

    void reset() override {
        // Trigger new excitation on next process() call
        mNeedsExcitation.store(true, std::memory_order_release);
        mFilterStateL = 0.0f;
        mFilterStateR = 0.0f;
    }

    void process(float* buffer, int32_t numFrames,
                 float frequency, float amplitude) override {
        // Read parameters with smoothing (prevents zipper noise)
        const float brightness = smoothParam(PARAM_BRIGHTNESS);
        const float decay = smoothParam(PARAM_DECAY);
        const float excitation = smoothParam(PARAM_EXCITATION);

        // Clamp frequency to valid range
        frequency = std::clamp(frequency, 20.0f, 20000.0f);

        // Delay length in fractional samples
        const float delaySamples = static_cast<float>(mSampleRate) / frequency;

        // Feedback coefficient: maps decay [0,1] → [0.9, 0.999]
        const float feedbackCoeff = 0.9f + decay * 0.099f;

        // LP filter coefficient: brightness [0,1] → [0.1, 0.95]
        // Lower = darker (more filtering), Higher = brighter
        const float lpCoeff = 0.1f + brightness * 0.85f;

        // Check if we need a new excitation burst
        bool needsExcitation = mNeedsExcitation.load(std::memory_order_acquire);

        // Auto-retrigger: when energy in delay line drops below threshold
        // This gives continuous sound while touching the XY pad (bowed string feel)
        if (!needsExcitation && mExcitationRemaining <= 0) {
            mEnergyAccumulator += std::abs(mFilterStateL);
            mEnergySampleCount++;
            if (mEnergySampleCount >= mSampleRate / 20) { // Check every ~50ms
                float avgEnergy = mEnergyAccumulator / static_cast<float>(mEnergySampleCount);
                if (avgEnergy < 0.001f) {
                    needsExcitation = true; // Signal too quiet, re-trigger
                }
                mEnergyAccumulator = 0.0f;
                mEnergySampleCount = 0;
            }
        }

        // Retrigger on significant frequency change (user moved finger)
        if (mPrevFrequency > 0.0f && std::abs(frequency - mPrevFrequency) > mPrevFrequency * 0.05f) {
            needsExcitation = true;
        }
        mPrevFrequency = frequency;

        if (needsExcitation) {
            mNeedsExcitation.store(false, std::memory_order_release);
            mExcitationRemaining = static_cast<int>(delaySamples);
            mExcitationPhase = 0.0f;
            mEnergyAccumulator = 0.0f;
            mEnergySampleCount = 0;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            // Read from delay line (fractional for pitch accuracy)
            float delayed = mDelayLine.readInterpolated(delaySamples, DelayLine::Interpolation::LINEAR);

            // One-pole lowpass filter (string damping)
            // y[n] = coeff * x[n] + (1-coeff) * y[n-1]
            mFilterStateL = lpCoeff * delayed + (1.0f - lpCoeff) * mFilterStateL;

            // Generate excitation if in burst phase
            float exc = 0.0f;
            if (mExcitationRemaining > 0) {
                exc = generateExcitation(excitation);
                mExcitationRemaining--;
            }

            // Feedback: filtered delayed signal + excitation
            float feedback = exc + mFilterStateL * feedbackCoeff;

            // Soft-clip to prevent runaway feedback
            feedback = std::tanh(feedback);

            // Write to delay line
            mDelayLine.write(feedback);

            // Output (stereo, slight detuning for width)
            float sample = mFilterStateL * amplitude;

            // Sanitize output
            if (!std::isfinite(sample)) sample = 0.0f;

            buffer[i * 2]     = sample;
            buffer[i * 2 + 1] = sample;
        }
    }

    const char* getName() const override { return "Karplus-Strong"; }
    int getParameterCount() const override { return 3; }

    EngineParameterDef getParameterDef(int paramId) const override {
        switch (paramId) {
            case PARAM_BRIGHTNESS:
                return {"Brightness", "BRIGHT", 0.0f, 1.0f, 0.5f};
            case PARAM_DECAY:
                return {"Decay", "DECAY", 0.0f, 1.0f, 0.5f};
            case PARAM_EXCITATION:
                return {"Excitation", "EXCITE", 0.0f, 1.0f, 0.3f};
            default:
                return {"Unknown", "?", 0.0f, 1.0f, 0.0f};
        }
    }

private:
    DelayLine mDelayLine{50.0f, 48000.0f};  // Will be re-initialized in prepare()

    float mFilterStateL = 0.0f;
    float mFilterStateR = 0.0f;
    float mPrevFrequency = 0.0f;
    float mExcitationPhase = 0.0f;
    int mExcitationRemaining = 0;

    // Energy tracking for auto-retrigger
    float mEnergyAccumulator = 0.0f;
    int mEnergySampleCount = 0;

    std::atomic<bool> mNeedsExcitation{true};

    // Fast deterministic RNG (xorshift32) — RT-safe, no syscalls
    uint32_t mRngState = 12345;

    float fastRandom() {
        mRngState ^= mRngState << 13;
        mRngState ^= mRngState >> 17;
        mRngState ^= mRngState << 5;
        // Convert to [-1, 1] range
        return static_cast<float>(static_cast<int32_t>(mRngState)) / static_cast<float>(INT32_MAX);
    }

    /**
     * @brief Generate excitation sample based on type
     * @param type 0=noise, 0.5=impulse, 1=swept frequency
     */
    float generateExcitation(float type) {
        if (type < 0.33f) {
            // Pure noise burst
            return fastRandom() * 0.8f;
        } else if (type < 0.66f) {
            // Impulse + noise mix (punchy pluck)
            float noise = fastRandom() * 0.3f;
            float impulse = (mExcitationRemaining > 2) ? 0.0f : 1.0f;
            return noise + impulse * 0.7f;
        } else {
            // Swept tone burst (metallic/bell character)
            mExcitationPhase += 0.1f + type * 0.4f;
            if (mExcitationPhase > static_cast<float>(M_PI) * 2.0f) {
                mExcitationPhase -= static_cast<float>(M_PI) * 2.0f;
            }
            float swept = std::sin(mExcitationPhase) * 0.6f;
            float noise = fastRandom() * 0.2f;
            return swept + noise;
        }
    }
};
