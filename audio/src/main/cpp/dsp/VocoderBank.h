#ifndef VOCODERBANK_H
#define VOCODERBANK_H

#include <vector>
#include <array>
#include <atomic>
#include <cmath>
#include "BiquadFilter.h"
#include "EnvelopeFollower.h"
#include "DSPMath.h"

/**
 * @file VocoderBank.h
 * @brief Band-split filter bank for vocoder analysis and synthesis
 *
 * A vocoder works by:
 * 1. Splitting the modulator (voice/mic) into frequency bands
 * 2. Extracting the envelope of each band
 * 3. Splitting the carrier (synth) into the same frequency bands
 * 4. Applying the modulator's envelopes to the carrier's bands
 * 5. Summing the bands to create the output
 *
 * This class handles steps 1-2 (analysis) and 3-5 (synthesis).
 *
 * Thread-safe: Parameters can be updated from UI thread.
 */
class VocoderBank {
public:
    /// Maximum number of frequency bands
    static constexpr int MAX_BANDS = 32;

    /// Minimum number of frequency bands
    static constexpr int MIN_BANDS = 4;

    /// Default number of bands
    static constexpr int DEFAULT_BANDS = 16;

    /**
     * @brief Constructor
     * @param numBands Initial number of bands (4-32)
     */
    explicit VocoderBank(int numBands = DEFAULT_BANDS);

    /**
     * @brief Prepare for processing
     * @param sampleRate Sample rate in Hz
     */
    void prepare(float sampleRate);

    /**
     * @brief Set number of frequency bands
     * @param numBands Number of bands (4-32)
     *
     * More bands = more detail but higher CPU usage
     * Fewer bands = more robotic, "classic vocoder" sound
     */
    void setNumBands(int numBands);

    /**
     * @brief Get current number of bands
     */
    int getNumBands() const {
        return mNumBands.load(std::memory_order_relaxed);
    }

    /**
     * @brief Set formant shift
     * @param semitones Shift in semitones (-24 to +24)
     *
     * Positive = higher formants (chipmunk)
     * Negative = lower formants (deep voice)
     */
    void setFormantShift(float semitones);

    /**
     * @brief Set envelope attack time
     * @param attackMs Attack time in milliseconds
     */
    void setAttack(float attackMs);

    /**
     * @brief Set envelope release time
     * @param releaseMs Release time in milliseconds
     */
    void setRelease(float releaseMs);

    /**
     * @brief Analyze modulator signal and extract band envelopes
     * @param modulator Input modulator signal (mono)
     * @param numSamples Number of samples
     * @param envelopes Output array of band envelopes
     *
     * This processes the modulator through bandpass filters and
     * extracts the amplitude envelope of each band.
     */
    void analyze(const float* modulator, int numSamples,
                 std::array<float, MAX_BANDS>& envelopes);

    /**
     * @brief Synthesize output from carrier using band envelopes
     * @param carrier Input carrier signal (mono)
     * @param output Output buffer
     * @param numSamples Number of samples
     * @param envelopes Band envelopes from analyze()
     *
     * This applies the modulator's envelopes to the carrier's frequency bands.
     */
    void synthesize(const float* carrier, float* output, int numSamples,
                    const std::array<float, MAX_BANDS>& envelopes);

    /**
     * @brief Get center frequency of a band
     * @param bandIndex Band index (0 to numBands-1)
     * @return Center frequency in Hz
     */
    float getBandFrequency(int bandIndex) const;

    /**
     * @brief Get all band center frequencies
     * @return Vector of center frequencies
     */
    const std::vector<float>& getBandFrequencies() const {
        return mBandFrequencies;
    }

    /**
     * @brief Reset all filter and envelope states
     */
    void reset();

private:
    float mSampleRate{48000.0f};

    // Parameters (thread-safe)
    std::atomic<int> mNumBands{DEFAULT_BANDS};
    std::atomic<float> mFormantShiftSemitones{0.0f};
    std::atomic<float> mAttackMs{5.0f};
    std::atomic<float> mReleaseMs{50.0f};

    // Frequency band configuration
    std::vector<float> mBandFrequencies;    // Center frequencies
    std::vector<float> mBandQs;             // Q factors for each band

    // Analysis section (modulator processing)
    std::vector<BiquadFilter> mAnalysisFilters;     // Bandpass filters
    std::vector<EnvelopeFollower> mEnvelopeFollowers;

    // Synthesis section (carrier processing)
    std::vector<BiquadFilter> mSynthesisFilters;    // Bandpass filters

    // Working buffers (pre-allocated)
    std::vector<float> mBandBuffer;         // Single band output

    // Per-sample envelope storage for each band [band][sample]
    // This enables sample-by-sample envelope application for proper vocoder sound
    std::array<std::vector<float>, MAX_BANDS> mEnvelopeBuffers;

    /**
     * @brief Recalculate band frequencies using logarithmic spacing
     *
     * Uses mel-scale-like distribution for natural vocal frequency response.
     * Range: 100 Hz to 10000 Hz
     */
    void recalculateBandFrequencies();

    /**
     * @brief Update all filters with current parameters
     */
    void updateFilters();

    /**
     * @brief Apply formant shift to synthesis filters
     */
    void applyFormantShift();
};

#endif // VOCODERBANK_H
