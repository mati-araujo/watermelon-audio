#ifndef COMPRESSOREFFECT_H
#define COMPRESSOREFFECT_H

#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <cmath>

/**
 * @file CompressorEffect.h
 * @brief Professional dynamics compressor for guitar/audio
 *
 * Features:
 * - Threshold: Signal level where compression begins (-60 to 0 dB)
 * - Ratio: Amount of compression (1:1 to 20:1, infinity at 20:1)
 * - Attack: How fast the compressor reacts (0.1 to 100 ms)
 * - Release: How fast the compressor recovers (10 to 1000 ms)
 * - Makeup Gain: Output level compensation (-6 to +24 dB)
 * - Knee: Transition smoothness (0 = hard, 20 dB = soft)
 *
 * Use cases:
 * - Guitar sustain and punch
 * - Dynamic control for clean playing
 * - Limiting for output protection
 *
 * Thread-safe: All parameters use atomic operations.
 */
class CompressorEffect : public Effect {
public:
    /**
     * @brief Parameter IDs
     */
    enum Param {
        THRESHOLD = 0,    ///< -60 to 0 dB
        RATIO = 1,        ///< 1.0 to 20.0 (infinity = 20)
        ATTACK = 2,       ///< 0.1 to 100 ms
        RELEASE = 3,      ///< 10 to 1000 ms
        MAKEUP_GAIN = 4,  ///< -6 to +24 dB
        KNEE = 5,         ///< 0 (hard) to 20 dB (soft)
        PARAM_COUNT = 6
    };

    CompressorEffect();
    ~CompressorEffect() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    /// Limpia la envolvente y re-siembra el smoother de makeup (WD-3.2).
    void reset() override;

    /**
     * @brief Get current gain reduction for metering
     * @return Gain reduction in dB (negative value)
     */
    float getGainReduction() const { return mGainReductionDb.load(std::memory_order_relaxed); }

private:
    int mSampleRate{48000};

    // Parameters (atomic for RT safety)
    std::atomic<float> mThresholdDb{-20.0f};
    std::atomic<float> mRatio{4.0f};
    std::atomic<float> mAttackMs{10.0f};
    std::atomic<float> mReleaseMs{100.0f};
    std::atomic<float> mMakeupDb{0.0f};
    std::atomic<float> mKneeDb{6.0f};

    // State
    float mEnvelope = 0.0f;
    std::atomic<float> mGainReductionDb{0.0f};

    // Coefficients (calculated from parameters)
    float mAttackCoeff = 0.0f;
    float mReleaseCoeff = 0.0f;

    // Smoother for makeup gain (prevents clicks on gain changes)
    ParameterSmoother mMakeupSmoother{0.995f};

    void updateCoefficients();
    float computeGain(float inputDb) const;
};

#endif // COMPRESSOREFFECT_H
