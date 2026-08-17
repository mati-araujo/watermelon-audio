#ifndef AMPSIMULATOR_H
#define AMPSIMULATOR_H

#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <cmath>
#include <array>

/**
 * @file AmpSimulator.h
 * @brief Professional tube amplifier simulator
 *
 * Three-stage architecture:
 * 1. Pre-amp: Gain + tube saturation (model-dependent clipping)
 * 2. Tone Stack: 3-band EQ (Bass, Mid, Treble) with model-specific frequencies
 * 3. Power Amp: Presence boost + sag (power supply compression)
 *
 * Amp Models:
 * - CLEAN: Fender Twin style, soft clipping, linear response
 * - CRUNCH: Marshall JCM800 style, tube-like asymmetric saturation
 * - HIGH_GAIN: Mesa Rectifier style, aggressive cascaded saturation
 * - MODERN: 5150/6505 style, hard clipping + saturation
 *
 * Tone Stack Types:
 * - FENDER: Scooped mids (150/600/2500 Hz)
 * - MARSHALL: Mid-forward (200/1000/3500 Hz)
 * - VOX: Bright, chime-y (180/900/4000 Hz)
 * - MESA: Tight low-end (100/800/3000 Hz)
 *
 * Thread-safe: All parameters use atomic operations.
 */
class AmpSimulator : public Effect {
public:
    /**
     * @brief Amp model types
     */
    enum class AmpModel {
        CLEAN = 0,      ///< Fender Twin style - clean, soft saturation
        CRUNCH = 1,     ///< Marshall JCM800 style - tube crunch
        HIGH_GAIN = 2,  ///< Mesa Rectifier style - high gain saturation
        MODERN = 3      ///< 5150/6505 style - modern high gain
    };

    /**
     * @brief Tone stack EQ types
     */
    enum class ToneStackType {
        FENDER = 0,     ///< Scooped mids, bright highs
        MARSHALL = 1,   ///< Mid-forward, classic rock
        VOX = 2,        ///< Chime-y, bright, British
        MESA = 3        ///< Tight lows, aggressive mids
    };

    /**
     * @brief Parameter IDs (0-8)
     */
    enum Param {
        GAIN = 0,       ///< Pre-amp drive [0-100]
        BASS = 1,       ///< Low shelf EQ [0-100] -> -12 to +12 dB
        MID = 2,        ///< Peaking EQ [0-100] -> -12 to +12 dB
        TREBLE = 3,     ///< High shelf EQ [0-100] -> -12 to +12 dB
        PRESENCE = 4,   ///< Power amp high freq boost [0-100] -> 0-30%
        MASTER = 5,     ///< Output level [0-100]
        SAG = 6,        ///< Power supply compression [0-100]
        AMP_MODEL = 7,  ///< Model selection [0-3]
        TONESTACK = 8,  ///< Tone stack type [0-3]
        PARAM_COUNT = 9
    };

    AmpSimulator();
    ~AmpSimulator() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

    /// Limpia los cuatro biquads por canal, el sag y los smoothers (WD-3.2).
    void reset() override;

private:
    int mSampleRate{48000};

    // Parameters (atomic for RT safety)
    std::atomic<float> mGain{50.0f};
    std::atomic<float> mBass{50.0f};
    std::atomic<float> mMid{50.0f};
    std::atomic<float> mTreble{50.0f};
    std::atomic<float> mPresence{50.0f};
    std::atomic<float> mMaster{75.0f};
    std::atomic<float> mSag{30.0f};
    std::atomic<int> mAmpModel{static_cast<int>(AmpModel::CRUNCH)};
    std::atomic<int> mToneStack{static_cast<int>(ToneStackType::MARSHALL)};

    // BiQuad filter state (per channel, 4 bands)
    struct BiQuadState {
        float x1 = 0.0f, x2 = 0.0f;  // Input history
        float y1 = 0.0f, y2 = 0.0f;  // Output history
    };

    // 4 filter bands (bass, mid, treble, presence) x 2 channels
    std::array<BiQuadState, 4> mFilterStateL;
    std::array<BiQuadState, 4> mFilterStateR;

    // BiQuad coefficients (a0, a1, a2, b0, b1, b2)
    struct BiQuadCoeffs {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;  // a0 normalized to 1
    };
    std::array<BiQuadCoeffs, 4> mFilterCoeffs;

    // Parameter smoothers (prevent clicks on gain/master changes)
    ParameterSmoother mGainSmoother{0.995f};
    ParameterSmoother mMasterSmoother{0.995f};

    // Sag envelope state
    float mSagEnvelope = 0.0f;
    float mSagAttackCoeff = 0.0f;
    float mSagReleaseCoeff = 0.0f;

    // Pre-amp stage processing
    float processPreamp(float input, AmpModel model, float gain);

    // Tone stack (3-band EQ)
    float processToneStack(float input, int channel);

    // Power amp stage
    float processPowerAmp(float input, float presence, float sag);

    // Saturation algorithms
    float softClip(float x);
    float hardClip(float x);
    float tubeSimulation(float x, float drive);

    // Filter coefficient calculation
    void updateFilterCoefficients();
    void calculateLowShelf(int index, float freq, float gainDb, float q = 0.707f);
    void calculatePeaking(int index, float freq, float gainDb, float q = 1.0f);
    void calculateHighShelf(int index, float freq, float gainDb, float q = 0.707f);

    // BiQuad processing
    float processBiQuad(float input, BiQuadState& state, const BiQuadCoeffs& coeffs);

    // Get tone stack frequencies based on type
    void getToneStackFrequencies(ToneStackType type, float& bassFreq, float& midFreq, float& trebleFreq);
};

#endif // AMPSIMULATOR_H
