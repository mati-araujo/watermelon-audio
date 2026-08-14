#ifndef LOOKAHEAD_LIMITER_H
#define LOOKAHEAD_LIMITER_H

#include "Effect.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>

/**
 * @file LookaheadLimiter.h
 * @brief Professional lookahead limiter for transparent peak limiting
 *
 * PHASE 4: Lookahead Limiter - prevents clipping with transparent limiting.
 * Uses a small lookahead buffer (default 5ms) to anticipate transients
 * and smoothly reduce gain before they clip.
 *
 * Features:
 * - True peak limiting with lookahead
 * - Configurable threshold (default -0.5dB)
 * - Fast attack (1ms) for transient catching
 * - Smooth release (100ms) for transparent recovery
 * - Zero latency compensation option
 */
class LookaheadLimiter : public Effect {
public:
    /**
     * @brief Constructor
     */
    LookaheadLimiter();

    /**
     * @brief Initialize the limiter with sample rate
     * @param sampleRate Sample rate in Hz
     */
    void prepare(int32_t sampleRate);

    /**
     * @brief Process audio through the limiter
     * @param input Input buffer (stereo interleaved)
     * @param output Output buffer (stereo interleaved)
     * @param numFrames Number of frames to process
     */
    void process(float* input, float* output, int numFrames) override;

    /**
     * @brief Clear the lookahead delay line and the gain envelope.
     *
     * This is the case Effect::reset() names in its own documentation — a delay
     * line that carries audio across a context change — and it was the one that
     * never implemented it. The line holds 5 ms, so without this a mode switch
     * or a stop/start hands the first block of the new context a tail of the
     * previous one.
     */
    void reset() override;

    /**
     * @brief El lookahead ES latencia sobre la señal directa (WD-3.1).
     *
     * `process()` saca el buffer DEMORADO multiplicado por la ganancia, no la
     * entrada: lo que entra en el sample n sale en el n + mLookaheadSamples.
     * Son 5 ms — 240 samples a 48 kHz, 480 a 96 kHz.
     *
     * Hasta WD-2.2 esto devolvia el 0 del default de `Effect`, y no rompia nada
     * porque el limiter vive en `OutputStage`, en el bus master, donde no hay
     * ramas que sumar (el razonamiento entero esta en `test_effect_latency.cpp`,
     * que por eso mismo NO lo alcanza: no esta en `EffectRegistry`).
     *
     * Pero era una declaracion falsa, y el contrato de WD-3.1 es que lo
     * declarado sea lo que se tiene. El dia que alguien lo registre como efecto
     * de cadena, la compensacion de ramas alinearia contra ese cero.
     */
    int getLatencySamples() const override { return mLookaheadSamples; }

    /**
     * @brief Set a parameter
     * @param paramId Parameter ID (0: threshold, 1: attack, 2: release, 3: lookahead)
     * @param value Parameter value
     */
    void setParam(int paramId, float value) override;

    /**
     * @brief Get a parameter value
     * @param paramId Parameter ID
     * @return Parameter value
     */
    float getParam(int paramId) override;

    /**
     * @brief Set sample rate
     * @param sampleRate Sample rate in Hz
     */
    void setSampleRate(int sampleRate) override;

    /**
     * @brief Set threshold in dB
     * @param thresholdDb Threshold level in dB (typically -3.0 to 0.0)
     */
    void setThreshold(float thresholdDb);

    /**
     * @brief Set attack time
     * @param attackMs Attack time in milliseconds (0.1 to 10.0)
     */
    void setAttack(float attackMs);

    /**
     * @brief Set release time
     * @param releaseMs Release time in milliseconds (10.0 to 1000.0)
     */
    void setRelease(float releaseMs);

    /**
     * @brief Get current gain reduction in dB
     * @return Current gain reduction (0 = no reduction)
     */
    float getGainReduction() const;

private:
    // Sample rate
    int32_t mSampleRate{48000};

    // Lookahead delay buffer (stereo)
    std::vector<float> mDelayBuffer;
    int32_t mWritePos{0};
    int32_t mLookaheadSamples{0};

    // Parameters (atomic for thread-safe UI updates)
    std::atomic<float> mThresholdDb{-0.5f};    // Threshold in dB
    std::atomic<float> mAttackMs{1.0f};        // Attack time in ms
    std::atomic<float> mReleaseMs{100.0f};     // Release time in ms

    // Internal state
    float mThresholdLinear{0.944f};  // -0.5dB in linear
    float mGain{1.0f};               // Current gain multiplier
    float mAttackCoeff{0.0f};        // Attack smoothing coefficient
    float mReleaseCoeff{0.0f};       // Release smoothing coefficient

    // For metering
    std::atomic<float> mCurrentGainReduction{0.0f};

    // Constants
    static constexpr float LOOKAHEAD_MS = 5.0f;  // Fixed 5ms lookahead
    static constexpr float MIN_THRESHOLD_DB = -12.0f;
    static constexpr float MAX_THRESHOLD_DB = 0.0f;
    static constexpr float MIN_ATTACK_MS = 0.1f;
    static constexpr float MAX_ATTACK_MS = 10.0f;
    static constexpr float MIN_RELEASE_MS = 10.0f;
    static constexpr float MAX_RELEASE_MS = 1000.0f;

    /**
     * @brief Update envelope coefficients based on current parameters
     */
    void updateCoefficients();

    /**
     * @brief Convert dB to linear gain
     */
    static float dbToLinear(float db) {
        return std::pow(10.0f, db / 20.0f);
    }

    /**
     * @brief Convert linear gain to dB
     */
    static float linearToDb(float linear) {
        if (linear <= 1e-10f) return -100.0f;
        return 20.0f * std::log10(linear);
    }
};

#endif // LOOKAHEAD_LIMITER_H
