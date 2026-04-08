#ifndef DISTORTIONEFFECT_H
#define DISTORTIONEFFECT_H

#include "Effect.h"
#include "EffectTypes.h"
#include "../dsp/Oversampler.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/DSPMath.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <vector>
#include <cmath>

/**
 * @file DistortionEffect.h
 * @brief Professional pedal-emulation distortion with oversampling
 *
 * Emulates classic distortion pedals:
 *
 * OVERDRIVE:
 * - Tube Screamer (TS-808/TS9): Mid-hump, warm, smooth breakup
 * - Boss Overdrive (OD-1/SD-1): Bright, articulate, asymmetric
 * - Klon Centaur: Transparent, dynamic, clean blend
 * - OCD (Fulltone): Amp-like crunch, versatile
 *
 * DISTORTION:
 * - Boss DS-1: Aggressive, cutting, bright
 * - RAT: Gritty, saturated, variable filter
 * - Distortion+: Classic mid-focused
 * - Metal Zone: High-gain, parametric EQ
 *
 * FUZZ:
 * - Big Muff Pi: Massive sustain, creamy, mid-scoop
 * - Fuzz Face Germanium: Warm, organic, cleanup responsive
 * - Fuzz Face Silicon: Bright, aggressive, tight
 * - Octave Fuzz: Ring-mod octave-up effect
 *
 * SPECIAL:
 * - HM-2 Chainsaw: Swedish death metal tone
 * - Doom Fuzz: Crushing doom/stoner fuzz
 *
 * Features:
 * - 2x/4x oversampling for anti-aliasing
 * - Pedal-specific tone stacks
 * - Per-pedal parameter mappings (PARAM_A, PARAM_B, PARAM_C)
 * - Wet/dry mix with output level compensation
 * - Legacy algorithm support for backward compatibility
 *
 * Thread-safe: All parameters use atomic operations.
 */
class DistortionEffect : public Effect {
public:
    /**
     * @brief Parameter IDs
     *
     * Parameters 0-4 are universal (all pedals use them).
     * Parameters 5-7 are pedal-specific (meaning varies by algorithm).
     * Parameters 8+ are advanced controls.
     */
    enum Param {
        // === Universal Parameters ===
        DRIVE = 0,              ///< Input drive/gain (0.0-1.0)
        TONE = 1,               ///< Tone control (0.0-1.0, dark to bright)
        LEVEL = 2,              ///< Output level (0.0-1.0)
        MIX = 3,                ///< Wet/dry mix (0.0-1.0)
        ALGORITHM = 4,          ///< Pedal type (see DistortionVariants)

        // === Pedal-Specific Parameters ===
        PARAM_A = 5,            ///< Pedal-specific param A (varies by pedal)
        PARAM_B = 6,            ///< Pedal-specific param B (varies by pedal)
        PARAM_C = 7,            ///< Pedal-specific param C (varies by pedal)

        // === Advanced Parameters ===
        OVERSAMPLE = 8,         ///< Oversampling factor (0=off, 1=2x, 2=4x)
        PRE_LOW_CUT = 9,        ///< Pre-distortion low cut (20-500 Hz)
        POST_HIGH_CUT = 10,     ///< Post-distortion high cut (1k-20k Hz)
        SAG = 11,               ///< Voltage sag simulation (0.0-1.0)
        BIAS = 12,              ///< Transistor bias (for fuzz, 0.0-1.0)
        GATE_THRESHOLD = 13,    ///< Noise gate threshold (0.0-1.0, 0=off)

        PARAM_COUNT = 14        ///< Total number of parameters
    };

    /**
     * @brief Pedal-specific parameter meanings
     *
     * TUBE_SCREAMER:
     *   PARAM_A = Mid frequency (520-920 Hz)
     *   PARAM_B = Mid Q/width (0.5-1.0)
     *
     * KLON:
     *   PARAM_A = Treble boost (0.0-1.0)
     *   PARAM_B = Clean blend (0.0-1.0)
     *
     * RAT:
     *   PARAM_A = Filter cutoff (dark to bright)
     *   PARAM_B = Turbo mode (0=normal, 1=LED clipping)
     *
     * BIG_MUFF:
     *   PARAM_A = Sustain amount (0.0-1.0)
     *   PARAM_B = Mid scoop depth (0.0-1.0)
     *
     * FUZZ_FACE:
     *   PARAM_A = Bias adjust (0.0-1.0)
     *   PARAM_B = Cleanup response (0.0-1.0)
     *
     * METAL_ZONE:
     *   PARAM_A = Low EQ (+/- 12dB)
     *   PARAM_B = High EQ (+/- 12dB)
     *   PARAM_C = Mid frequency (200-3000 Hz)
     *
     * HM2_CHAINSAW:
     *   PARAM_A = Low boost (0-12 dB)
     *   PARAM_B = High boost (0-12 dB)
     */

    /**
     * @brief Constructor
     */
    DistortionEffect();

    /**
     * @brief Destructor
     */
    ~DistortionEffect() override = default;

    /**
     * @brief Process audio through distortion
     * @param input Input buffer (stereo interleaved)
     * @param output Output buffer (stereo interleaved)
     * @param numFrames Number of frames to process
     */
    void process(float* input, float* output, int numFrames) override;

    /**
     * @brief Set a parameter value
     * @param paramId Parameter ID from Param enum
     * @param value Parameter value
     */
    void setParam(int paramId, float value) override;

    /**
     * @brief Get a parameter value
     * @param paramId Parameter ID from Param enum
     * @return Current parameter value
     */
    float getParam(int paramId) override;

    /**
     * @brief Set sample rate
     * @param sampleRate Sample rate in Hz
     */
    void setSampleRate(int sampleRate) override;

private:
    int mSampleRate{48000};

    // ========== Universal Parameters (atomic for thread-safety) ==========
    std::atomic<float> mDrive{0.5f};
    std::atomic<float> mTone{0.5f};
    std::atomic<float> mLevel{0.7f};           // Output level (0-1)
    std::atomic<float> mMix{1.0f};
    std::atomic<int> mAlgorithm{DistortionVariants::TUBE_SCREAMER};

    // ========== Pedal-Specific Parameters ==========
    std::atomic<float> mParamA{0.5f};          // Varies by pedal
    std::atomic<float> mParamB{0.5f};          // Varies by pedal
    std::atomic<float> mParamC{0.5f};          // Varies by pedal

    // ========== Advanced Parameters ==========
    std::atomic<int> mOversampleFactor{1};     // 0=1x, 1=2x, 2=4x
    std::atomic<float> mPreLowCut{80.0f};
    std::atomic<float> mPostHighCut{12000.0f};
    std::atomic<float> mSag{0.0f};             // Voltage sag (0-1)
    std::atomic<float> mBias{0.5f};            // Transistor bias
    std::atomic<float> mGateThreshold{0.0f};   // Noise gate (0=off)

    // ========== Parameter Smoothers (prevent clicks on changes) ==========
    ParameterSmoother mDriveSmoother{0.995f};   // ~10ms at 48kHz
    ParameterSmoother mLevelSmoother{0.995f};   // ~10ms at 48kHz
    ParameterSmoother mMixSmoother{0.995f};     // ~10ms at 48kHz

    // ========== Internal State ==========
    float mLastSlewL{0.0f};                    // For RAT slew rate limiter
    float mLastSlewR{0.0f};
    float mSagVoltage{1.0f};                   // Current simulated voltage
    float mOctavePhaseL{0.0f};                 // For octave fuzz
    float mOctavePhaseR{0.0f};

    // Oversampler for anti-aliasing
    Oversampler mOversamplerL;
    Oversampler mOversamplerR;

    // ========== Tone Stack Filters ==========
    // Universal filters
    BiquadFilter mPreHPF_L;         ///< Pre-distortion high-pass (low cut)
    BiquadFilter mPreHPF_R;
    BiquadFilter mPostLPF_L;        ///< Post-distortion low-pass (high cut)
    BiquadFilter mPostLPF_R;

    // Tube Screamer mid-hump filters
    BiquadFilter mTSMidBoost_L;     ///< TS mid-boost peaking
    BiquadFilter mTSMidBoost_R;

    // RAT filter control
    BiquadFilter mRATFilter_L;      ///< RAT variable LPF
    BiquadFilter mRATFilter_R;

    // Big Muff tone stack
    BiquadFilter mMuffToneLPF_L;    ///< Big Muff tone LPF
    BiquadFilter mMuffToneLPF_R;
    BiquadFilter mMuffToneHPF_L;    ///< Big Muff tone HPF
    BiquadFilter mMuffToneHPF_R;
    BiquadFilter mMuffMidScoop_L;   ///< Big Muff mid scoop notch
    BiquadFilter mMuffMidScoop_R;

    // HM-2 EQ stack
    BiquadFilter mHM2LowShelf_L;    ///< HM-2 low boost
    BiquadFilter mHM2LowShelf_R;
    BiquadFilter mHM2HighShelf_L;   ///< HM-2 high boost
    BiquadFilter mHM2HighShelf_R;
    BiquadFilter mHM2MidScoop_L;    ///< HM-2 mid cut
    BiquadFilter mHM2MidScoop_R;
    BiquadFilter mHM2Presence_L;    ///< HM-2 presence boost
    BiquadFilter mHM2Presence_R;

    // Metal Zone parametric EQ
    BiquadFilter mMTLowShelf_L;
    BiquadFilter mMTLowShelf_R;
    BiquadFilter mMTHighShelf_L;
    BiquadFilter mMTHighShelf_R;
    BiquadFilter mMTMidPeak_L;
    BiquadFilter mMTMidPeak_R;

    // Generic tone filters
    BiquadFilter mPreTone_L;        ///< Pre-distortion tone shaping
    BiquadFilter mPreTone_R;
    BiquadFilter mPostTone_L;       ///< Post-distortion tone shaping
    BiquadFilter mPostTone_R;

    // Working buffers (pre-allocated to avoid RT allocations)
    std::vector<float> mUpsampledL;
    std::vector<float> mUpsampledR;
    std::vector<float> mProcessedL;
    std::vector<float> mProcessedR;
    std::vector<float> mInputL;
    std::vector<float> mInputR;
    std::vector<float> mDryL;
    std::vector<float> mDryR;
    std::vector<float> mOutputL;
    std::vector<float> mOutputR;

    // Bitcrusher state (legacy)
    float mBitcrushHoldL{0.0f};
    float mBitcrushHoldR{0.0f};
    int mBitcrushCounter{0};

    // ========== Core Processing ==========

    /**
     * @brief Apply selected pedal algorithm to a sample
     * @param input Input sample
     * @param drive Drive amount (0.0-1.0)
     * @param algorithm Pedal type from DistortionVariants
     * @return Distorted sample
     */
    float applyDistortion(float input, float drive, int algorithm);

    // ========== OVERDRIVE Pedals ==========

    /** @brief Tube Screamer (TS-808/TS9) - mid-hump, warm breakup */
    float processTubeScreamer(float input, float drive);

    /** @brief Boss OD-1/SD-1 - bright, articulate */
    float processBossOverdrive(float input, float drive);

    /** @brief Klon Centaur - transparent, dynamic */
    float processKlon(float input, float drive);

    /** @brief Fulltone OCD - amp-like crunch */
    float processOCD(float input, float drive);

    // ========== DISTORTION Pedals ==========

    /** @brief Boss DS-1 - aggressive, cutting */
    float processBossDS1(float input, float drive);

    /** @brief ProCo RAT - gritty, saturated */
    float processRAT(float input, float drive, bool isLeft);

    /** @brief MXR Distortion+ - classic, mid-focused */
    float processDistortionPlus(float input, float drive);

    /** @brief Boss MT-2 Metal Zone - high-gain, scooped */
    float processMetalZone(float input, float drive);

    // ========== FUZZ Pedals ==========

    /** @brief EHX Big Muff Pi - massive sustained fuzz */
    float processBigMuff(float input, float drive);

    /** @brief Fuzz Face - warm organic fuzz
     *  @param germanium true for germanium, false for silicon */
    float processFuzzFace(float input, float drive, bool germanium);

    /** @brief Octavia - ring-mod octave-up fuzz */
    float processOctaveFuzz(float input, float drive, float& phase);

    // ========== SPECIAL Pedals ==========

    /** @brief Boss HM-2 - Swedish death metal chainsaw */
    float processHM2(float input, float drive);

    /** @brief Doom/Sunn fuzz - crushing doom fuzz */
    float processDoomFuzz(float input, float drive);

    // ========== LEGACY Algorithms (backward compatibility) ==========

    /** @brief Soft clip using tanh saturation */
    float processSoftClip(float input, float drive);

    /** @brief Hard clip with polynomial knee */
    float processHardClip(float input, float drive);

    /** @brief Asymmetric tube simulation */
    float processTubeSim(float input, float drive);

    /** @brief Wave folding distortion */
    float processFoldback(float input, float drive);

    /** @brief Bit reduction */
    float processBitcrush(float input, float drive);

    // ========== Utility Functions ==========

    /** @brief Update filter coefficients based on current parameters and algorithm */
    void updateFilters();

    /** @brief Update filters for specific pedal type */
    void updateFiltersForPedal(int algorithm);

    /** @brief Symmetric diode clipping (like TS-808) */
    float symmetricDiodeClip(float input, float threshold = 0.4f);

    /** @brief Asymmetric diode clipping (like DS-1) */
    float asymmetricDiodeClip(float input);

    /** @brief LED clipping (higher threshold, like RAT turbo) */
    float ledClip(float input);

    /** @brief Transistor saturation model */
    float transistorSaturate(float input, float vbe, float hfe);

    /** @brief Apply voltage sag effect */
    float applySag(float drive);

    /** @brief Noise gate */
    float applyGate(float input, float threshold);
};

#endif // DISTORTIONEFFECT_H
