#ifndef PARAMETRIC_EQ_H
#define PARAMETRIC_EQ_H

#include "Effect.h"
#include "../dsp/BiquadFilter.h"
#include <array>
#include <atomic>

/**
 * @file ParametricEQ.h
 * @brief Professional 3-band parametric equalizer
 *
 * PHASE 5: Pro Features - Parametric EQ
 *
 * 3-band EQ with:
 * - Low shelf (bass control)
 * - Mid peaking band (parametric)
 * - High shelf (treble control)
 *
 * Each band is stereo and processes L/R independently.
 * All parameters are thread-safe for real-time UI updates.
 */
class ParametricEQ : public Effect {
public:
    /**
     * @brief EQ band identifiers
     */
    enum Band {
        LOW = 0,    ///< Low shelf band
        MID = 1,    ///< Parametric mid band
        HIGH = 2,   ///< High shelf band
        NUM_BANDS = 3
    };

    /**
     * @brief Constructor
     */
    ParametricEQ();

    /**
     * @brief Process audio through the EQ
     * @param input Input buffer (stereo interleaved)
     * @param output Output buffer (stereo interleaved)
     * @param numFrames Number of frames to process
     */
    void process(float* input, float* output, int numFrames) override;

    /**
     * @brief Set a parameter
     * @param paramId Parameter ID (see parameter mapping below)
     * @param value Parameter value
     *
     * Parameter mapping:
     *   0-2: Band frequency (LOW, MID, HIGH)
     *   3-5: Band gain in dB (LOW, MID, HIGH)
     *   6:   Mid band Q
     *   7-9: Band bypass (LOW, MID, HIGH) - 0=active, 1=bypass
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

    /// Limpia la memoria de los seis biquads (WD-3.2).
    void reset() override;

    // ============== Band-specific setters ==============

    /**
     * @brief Set low shelf parameters
     * @param frequency Cutoff frequency in Hz (20-500)
     * @param gainDb Gain in dB (-15 to +15)
     */
    void setLowShelf(float frequency, float gainDb);

    /**
     * @brief Set mid band parameters
     * @param frequency Center frequency in Hz (100-10000)
     * @param gainDb Gain in dB (-15 to +15)
     * @param q Q factor (0.5 to 10)
     */
    void setMid(float frequency, float gainDb, float q);

    /**
     * @brief Set high shelf parameters
     * @param frequency Cutoff frequency in Hz (2000-20000)
     * @param gainDb Gain in dB (-15 to +15)
     */
    void setHighShelf(float frequency, float gainDb);

    /**
     * @brief Enable or bypass a specific band
     * @param band Band to control
     * @param bypass true to bypass (pass-through), false to enable
     */
    void setBandBypass(Band band, bool bypass);

    /**
     * @brief Check if a band is bypassed
     * @param band Band to check
     * @return true if bypassed, false if active
     */
    bool isBandBypassed(Band band) const;

private:
    // Sample rate
    int mSampleRate{48000};

    // Stereo filter pairs for each band (L and R)
    std::array<BiquadFilter, NUM_BANDS> mFiltersL;
    std::array<BiquadFilter, NUM_BANDS> mFiltersR;

    // Band parameters (atomic for thread-safe updates)
    std::array<std::atomic<float>, NUM_BANDS> mFrequency;
    std::array<std::atomic<float>, NUM_BANDS> mGainDb;
    std::atomic<float> mMidQ{1.0f};

    // Band bypass flags
    std::array<std::atomic<bool>, NUM_BANDS> mBypassed;

    // Default frequencies
    static constexpr float DEFAULT_LOW_FREQ = 100.0f;
    static constexpr float DEFAULT_MID_FREQ = 1000.0f;
    static constexpr float DEFAULT_HIGH_FREQ = 8000.0f;

    // Frequency ranges
    static constexpr float LOW_FREQ_MIN = 20.0f;
    static constexpr float LOW_FREQ_MAX = 500.0f;
    static constexpr float MID_FREQ_MIN = 100.0f;
    static constexpr float MID_FREQ_MAX = 10000.0f;
    static constexpr float HIGH_FREQ_MIN = 2000.0f;
    static constexpr float HIGH_FREQ_MAX = 20000.0f;

    // Gain range
    static constexpr float GAIN_MIN = -15.0f;
    static constexpr float GAIN_MAX = 15.0f;

    // Q range for mid band
    static constexpr float Q_MIN = 0.5f;
    static constexpr float Q_MAX = 10.0f;

    // Shelf Q (fixed slope)
    static constexpr float SHELF_Q = 0.707f;

    /**
     * @brief Update filter coefficients for a specific band
     */
    void updateBand(Band band);
};

#endif // PARAMETRIC_EQ_H
