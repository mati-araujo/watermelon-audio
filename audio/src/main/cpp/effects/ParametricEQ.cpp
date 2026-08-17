#include "ParametricEQ.h"
#include "EffectDefaults.h"
#include <algorithm>

ParametricEQ::ParametricEQ()
    : mFiltersL{BiquadFilter(DEFAULT_SAMPLE_RATE), BiquadFilter(DEFAULT_SAMPLE_RATE), BiquadFilter(DEFAULT_SAMPLE_RATE)},
      mFiltersR{BiquadFilter(DEFAULT_SAMPLE_RATE), BiquadFilter(DEFAULT_SAMPLE_RATE), BiquadFilter(DEFAULT_SAMPLE_RATE)} {

    // Initialize frequencies
    mFrequency[LOW].store(DEFAULT_LOW_FREQ, std::memory_order_relaxed);
    mFrequency[MID].store(DEFAULT_MID_FREQ, std::memory_order_relaxed);
    mFrequency[HIGH].store(DEFAULT_HIGH_FREQ, std::memory_order_relaxed);

    // Initialize gains (flat EQ by default)
    for (int i = 0; i < NUM_BANDS; ++i) {
        mGainDb[i].store(0.0f, std::memory_order_relaxed);
        mBypassed[i].store(false, std::memory_order_relaxed);
    }

    mMidQ.store(1.0f, std::memory_order_relaxed);
    mSampleRate = DEFAULT_SAMPLE_RATE;

    // Configure initial filter types
    updateBand(LOW);
    updateBand(MID);
    updateBand(HIGH);
}

void ParametricEQ::process(float* input, float* output, int numFrames) {
    // Check if all bands are bypassed (early exit)
    bool allBypassed = mBypassed[LOW].load(std::memory_order_relaxed) &&
                       mBypassed[MID].load(std::memory_order_relaxed) &&
                       mBypassed[HIGH].load(std::memory_order_relaxed);

    if (allBypassed) {
        // Pass-through: copy input to output
        for (int i = 0; i < numFrames * 2; ++i) {
            output[i] = input[i];
        }
        return;
    }

    // Process each frame through active bands
    // We process sample-by-sample for each band to maintain filter state correctly
    for (int i = 0; i < numFrames; ++i) {
        float sampleL = input[i * 2];
        float sampleR = input[i * 2 + 1];

        // Low shelf band
        if (!mBypassed[LOW].load(std::memory_order_relaxed)) {
            sampleL = mFiltersL[LOW].process(sampleL);
            sampleR = mFiltersR[LOW].process(sampleR);
        }

        // Mid peaking band
        if (!mBypassed[MID].load(std::memory_order_relaxed)) {
            sampleL = mFiltersL[MID].process(sampleL);
            sampleR = mFiltersR[MID].process(sampleR);
        }

        // High shelf band
        if (!mBypassed[HIGH].load(std::memory_order_relaxed)) {
            sampleL = mFiltersL[HIGH].process(sampleL);
            sampleR = mFiltersR[HIGH].process(sampleR);
        }

        output[i * 2] = sampleL;
        output[i * 2 + 1] = sampleR;
    }
}

void ParametricEQ::setParam(int paramId, float value) {
    switch (paramId) {
        // Frequencies (0-2)
        case 0:
            setLowShelf(value, mGainDb[LOW].load(std::memory_order_relaxed));
            break;
        case 1:
            setMid(value, mGainDb[MID].load(std::memory_order_relaxed),
                   mMidQ.load(std::memory_order_relaxed));
            break;
        case 2:
            setHighShelf(value, mGainDb[HIGH].load(std::memory_order_relaxed));
            break;

        // Gains (3-5)
        case 3:
            setLowShelf(mFrequency[LOW].load(std::memory_order_relaxed), value);
            break;
        case 4:
            setMid(mFrequency[MID].load(std::memory_order_relaxed), value,
                   mMidQ.load(std::memory_order_relaxed));
            break;
        case 5:
            setHighShelf(mFrequency[HIGH].load(std::memory_order_relaxed), value);
            break;

        // Mid Q (6)
        case 6:
            setMid(mFrequency[MID].load(std::memory_order_relaxed),
                   mGainDb[MID].load(std::memory_order_relaxed), value);
            break;

        // Bypass flags (7-9)
        case 7:
            setBandBypass(LOW, value > 0.5f);
            break;
        case 8:
            setBandBypass(MID, value > 0.5f);
            break;
        case 9:
            setBandBypass(HIGH, value > 0.5f);
            break;

        default:
            break;
    }
}

float ParametricEQ::getParam(int paramId) {
    switch (paramId) {
        case 0: return mFrequency[LOW].load(std::memory_order_relaxed);
        case 1: return mFrequency[MID].load(std::memory_order_relaxed);
        case 2: return mFrequency[HIGH].load(std::memory_order_relaxed);
        case 3: return mGainDb[LOW].load(std::memory_order_relaxed);
        case 4: return mGainDb[MID].load(std::memory_order_relaxed);
        case 5: return mGainDb[HIGH].load(std::memory_order_relaxed);
        case 6: return mMidQ.load(std::memory_order_relaxed);
        case 7: return mBypassed[LOW].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        case 8: return mBypassed[MID].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        case 9: return mBypassed[HIGH].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        default: return 0.0f;
    }
}

void ParametricEQ::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;

    // Update all filters with new sample rate
    for (int i = 0; i < NUM_BANDS; ++i) {
        mFiltersL[i].setSampleRate(static_cast<float>(sampleRate));
        mFiltersR[i].setSampleRate(static_cast<float>(sampleRate));
    }

    // Reconfigure all bands
    updateBand(LOW);
    updateBand(MID);
    updateBand(HIGH);
}

void ParametricEQ::setLowShelf(float frequency, float gainDb) {
    frequency = std::clamp(frequency, LOW_FREQ_MIN, LOW_FREQ_MAX);
    gainDb = std::clamp(gainDb, GAIN_MIN, GAIN_MAX);

    mFrequency[LOW].store(frequency, std::memory_order_relaxed);
    mGainDb[LOW].store(gainDb, std::memory_order_relaxed);

    updateBand(LOW);
}

void ParametricEQ::setMid(float frequency, float gainDb, float q) {
    frequency = std::clamp(frequency, MID_FREQ_MIN, MID_FREQ_MAX);
    gainDb = std::clamp(gainDb, GAIN_MIN, GAIN_MAX);
    q = std::clamp(q, Q_MIN, Q_MAX);

    mFrequency[MID].store(frequency, std::memory_order_relaxed);
    mGainDb[MID].store(gainDb, std::memory_order_relaxed);
    mMidQ.store(q, std::memory_order_relaxed);

    updateBand(MID);
}

void ParametricEQ::setHighShelf(float frequency, float gainDb) {
    frequency = std::clamp(frequency, HIGH_FREQ_MIN, HIGH_FREQ_MAX);
    gainDb = std::clamp(gainDb, GAIN_MIN, GAIN_MAX);

    mFrequency[HIGH].store(frequency, std::memory_order_relaxed);
    mGainDb[HIGH].store(gainDb, std::memory_order_relaxed);

    updateBand(HIGH);
}

void ParametricEQ::reset() {
    // Seis biquads: tres bandas por dos canales. No se tocan frecuencias,
    // ganancias ni bypass — eso es configuracion, no estado.
    //
    // ParametricEQ NO esta en EffectRegistry, asi que el barrido property-based
    // de WD-2.2 no lo alcanza; entra por el virtual puro, que es justamente el
    // punto de haberlo hecho virtual puro.
    for (int i = 0; i < NUM_BANDS; ++i) {
        mFiltersL[i].reset();
        mFiltersR[i].reset();
    }
}

void ParametricEQ::setBandBypass(Band band, bool bypass) {
    if (band < NUM_BANDS) {
        mBypassed[band].store(bypass, std::memory_order_relaxed);
    }
}

bool ParametricEQ::isBandBypassed(Band band) const {
    if (band < NUM_BANDS) {
        return mBypassed[band].load(std::memory_order_relaxed);
    }
    return false;
}

void ParametricEQ::updateBand(Band band) {
    float freq = mFrequency[band].load(std::memory_order_relaxed);
    float gain = mGainDb[band].load(std::memory_order_relaxed);

    switch (band) {
        case LOW:
            mFiltersL[LOW].setLowShelf(freq, SHELF_Q, gain);
            mFiltersR[LOW].setLowShelf(freq, SHELF_Q, gain);
            break;

        case MID: {
            float q = mMidQ.load(std::memory_order_relaxed);
            mFiltersL[MID].setPeaking(freq, q, gain);
            mFiltersR[MID].setPeaking(freq, q, gain);
            break;
        }

        case HIGH:
            mFiltersL[HIGH].setHighShelf(freq, SHELF_Q, gain);
            mFiltersR[HIGH].setHighShelf(freq, SHELF_Q, gain);
            break;

        default:
            break;
    }
}
