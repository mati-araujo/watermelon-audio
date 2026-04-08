#include "VocoderBank.h"
#include <algorithm>
#include <cassert>

// Frequency range for vocoder bands
constexpr float MIN_FREQ = 100.0f;   // Lowest band center
constexpr float MAX_FREQ = 10000.0f; // Highest band center

VocoderBank::VocoderBank(int numBands) {
    // Clamp to valid range
    numBands = std::clamp(numBands, MIN_BANDS, MAX_BANDS);
    mNumBands.store(numBands, std::memory_order_relaxed);

    // Pre-allocate for maximum bands
    mBandFrequencies.resize(MAX_BANDS);
    mBandQs.resize(MAX_BANDS);

    mAnalysisFilters.reserve(MAX_BANDS);
    mSynthesisFilters.reserve(MAX_BANDS);
    mEnvelopeFollowers.reserve(MAX_BANDS);

    for (int i = 0; i < MAX_BANDS; ++i) {
        mAnalysisFilters.emplace_back(48000.0f);
        mSynthesisFilters.emplace_back(48000.0f);
        mEnvelopeFollowers.emplace_back();
    }

    mBandBuffer.resize(4096, 0.0f);  // Max block size

    // Pre-allocate envelope buffers for each band
    for (int i = 0; i < MAX_BANDS; ++i) {
        mEnvelopeBuffers[i].resize(4096, 0.0f);
    }

    recalculateBandFrequencies();
}

void VocoderBank::prepare(float sampleRate) {
    mSampleRate = sampleRate;

    // Update all filters with new sample rate
    for (int i = 0; i < MAX_BANDS; ++i) {
        mAnalysisFilters[i].setSampleRate(sampleRate);
        mSynthesisFilters[i].setSampleRate(sampleRate);
        mEnvelopeFollowers[i].prepare(sampleRate);
    }

    // Reconfigure with current parameters
    updateFilters();
}

void VocoderBank::setNumBands(int numBands) {
    numBands = std::clamp(numBands, MIN_BANDS, MAX_BANDS);
    int oldBands = mNumBands.exchange(numBands, std::memory_order_acq_rel);

    if (numBands != oldBands) {
        recalculateBandFrequencies();
        updateFilters();
    }
}

void VocoderBank::setFormantShift(float semitones) {
    semitones = std::clamp(semitones, -24.0f, 24.0f);
    mFormantShiftSemitones.store(semitones, std::memory_order_relaxed);
    applyFormantShift();
}

void VocoderBank::setAttack(float attackMs) {
    attackMs = std::clamp(attackMs, 0.1f, 100.0f);
    mAttackMs.store(attackMs, std::memory_order_relaxed);

    int numBands = mNumBands.load(std::memory_order_relaxed);
    for (int i = 0; i < numBands; ++i) {
        mEnvelopeFollowers[i].setAttack(attackMs);
    }
}

void VocoderBank::setRelease(float releaseMs) {
    releaseMs = std::clamp(releaseMs, 1.0f, 500.0f);
    mReleaseMs.store(releaseMs, std::memory_order_relaxed);

    int numBands = mNumBands.load(std::memory_order_relaxed);
    for (int i = 0; i < numBands; ++i) {
        mEnvelopeFollowers[i].setRelease(releaseMs);
    }
}

void VocoderBank::analyze(const float* modulator, int numSamples,
                          std::array<float, MAX_BANDS>& envelopes) {
    int numBands = mNumBands.load(std::memory_order_acquire);

    // Buffers pre-allocated to 4096 in constructor. Assert in debug builds.
    assert(static_cast<int>(mBandBuffer.size()) >= numSamples &&
           "Band buffer too small — pre-allocate larger in constructor");
    if (numSamples > static_cast<int>(mBandBuffer.size())) {
        numSamples = static_cast<int>(mBandBuffer.size());
    }

    // Process each band - extract per-sample envelopes
    for (int band = 0; band < numBands; ++band) {
        float envSum = 0.0f;

        // Filter modulator through bandpass AND extract envelope sample-by-sample
        for (int i = 0; i < numSamples; ++i) {
            float filtered = mAnalysisFilters[band].process(modulator[i]);
            float env = mEnvelopeFollowers[band].process(filtered);
            mEnvelopeBuffers[band][i] = env;  // Store per-sample envelope
            envSum += env;
        }

        // Also store average for compatibility (used for silent band detection)
        envelopes[band] = envSum / static_cast<float>(numSamples);
    }

    // Zero out unused bands
    for (int band = numBands; band < MAX_BANDS; ++band) {
        envelopes[band] = 0.0f;
    }
}

void VocoderBank::synthesize(const float* carrier, float* output, int numSamples,
                             const std::array<float, MAX_BANDS>& envelopes) {
    int numBands = mNumBands.load(std::memory_order_acquire);

    // Buffers pre-allocated to 4096 in constructor. Assert in debug builds.
    assert(static_cast<int>(mBandBuffer.size()) >= numSamples &&
           "Band buffer too small — pre-allocate larger in constructor");
    if (numSamples > static_cast<int>(mBandBuffer.size())) {
        numSamples = static_cast<int>(mBandBuffer.size());
    }

    // Clear output
    std::fill(output, output + numSamples, 0.0f);

    // Process each band and sum - apply per-sample envelopes
    for (int band = 0; band < numBands; ++band) {
        float avgEnvelope = envelopes[band];

        // Skip completely silent bands (optimization)
        if (avgEnvelope < DSPMath::EPSILON) {
            // Still process filter to maintain state
            for (int i = 0; i < numSamples; ++i) {
                mSynthesisFilters[band].process(carrier[i]);
            }
            continue;
        }

        // Filter carrier through bandpass AND apply per-sample envelope
        for (int i = 0; i < numSamples; ++i) {
            float filtered = mSynthesisFilters[band].process(carrier[i]);
            // Apply the per-sample envelope from analyze() - this is the key fix!
            output[i] += filtered * mEnvelopeBuffers[band][i];
        }
    }

    // Normalize output - adjust gain based on number of bands
    // Using slightly higher gain to compensate for envelope attenuation
    float normFactor = 3.0f / std::sqrt(static_cast<float>(numBands));
    for (int i = 0; i < numSamples; ++i) {
        output[i] *= normFactor;
    }
}

float VocoderBank::getBandFrequency(int bandIndex) const {
    if (bandIndex < 0 || bandIndex >= MAX_BANDS) {
        return 0.0f;
    }
    return mBandFrequencies[bandIndex];
}

void VocoderBank::reset() {
    for (int i = 0; i < MAX_BANDS; ++i) {
        mAnalysisFilters[i].reset();
        mSynthesisFilters[i].reset();
        mEnvelopeFollowers[i].reset();
        std::fill(mEnvelopeBuffers[i].begin(), mEnvelopeBuffers[i].end(), 0.0f);
    }
    std::fill(mBandBuffer.begin(), mBandBuffer.end(), 0.0f);
}

void VocoderBank::recalculateBandFrequencies() {
    int numBands = mNumBands.load(std::memory_order_relaxed);

    // Use logarithmic spacing for natural frequency distribution
    // Similar to mel scale, which matches human perception
    float logMin = std::log10(MIN_FREQ);
    float logMax = std::log10(MAX_FREQ);
    float logStep = (logMax - logMin) / static_cast<float>(numBands);

    for (int i = 0; i < numBands; ++i) {
        float logFreq = logMin + (static_cast<float>(i) + 0.5f) * logStep;
        mBandFrequencies[i] = std::pow(10.0f, logFreq);

        // Q is proportional to frequency for constant-Q filter bank
        // Higher Q for higher frequencies maintains musical intervals
        float bandwidth = std::pow(10.0f, logMin + (static_cast<float>(i) + 1) * logStep)
                        - std::pow(10.0f, logMin + static_cast<float>(i) * logStep);
        mBandQs[i] = mBandFrequencies[i] / bandwidth;

        // Limit Q to reasonable range
        mBandQs[i] = std::clamp(mBandQs[i], 1.0f, 20.0f);
    }

    updateFilters();
}

void VocoderBank::updateFilters() {
    int numBands = mNumBands.load(std::memory_order_relaxed);
    float attackMs = mAttackMs.load(std::memory_order_relaxed);
    float releaseMs = mReleaseMs.load(std::memory_order_relaxed);

    for (int i = 0; i < numBands; ++i) {
        // Set analysis filters (no formant shift)
        mAnalysisFilters[i].setBandpass(mBandFrequencies[i], mBandQs[i]);

        // Set synthesis filters (with formant shift)
        // Formant shift applied separately in applyFormantShift()
        mSynthesisFilters[i].setBandpass(mBandFrequencies[i], mBandQs[i]);

        // Configure envelope followers
        mEnvelopeFollowers[i].setAttack(attackMs);
        mEnvelopeFollowers[i].setRelease(releaseMs);
        mEnvelopeFollowers[i].setMode(EnvelopeFollower::Mode::RMS);
    }

    applyFormantShift();
}

void VocoderBank::applyFormantShift() {
    float shiftSemitones = mFormantShiftSemitones.load(std::memory_order_relaxed);

    if (std::abs(shiftSemitones) < 0.01f) {
        return; // No significant shift
    }

    int numBands = mNumBands.load(std::memory_order_relaxed);

    // Convert semitones to frequency ratio
    // 12 semitones = 1 octave = 2x frequency
    float ratio = std::pow(2.0f, shiftSemitones / 12.0f);

    // Apply shift to synthesis filters only
    // Analysis filters stay at original frequencies
    for (int i = 0; i < numBands; ++i) {
        float shiftedFreq = mBandFrequencies[i] * ratio;

        // Clamp to valid frequency range
        shiftedFreq = std::clamp(shiftedFreq, 20.0f, mSampleRate * 0.45f);

        mSynthesisFilters[i].setBandpass(shiftedFreq, mBandQs[i]);
    }
}
