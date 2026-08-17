#include "AmpSimulator.h"
#include <algorithm>
#include "../platform/Logger.h"

#define LOG_TAG "AmpSimulator"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)

// Constants for M_PI if not defined
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

AmpSimulator::AmpSimulator() {
    // Initialize filter states
    for (auto& state : mFilterStateL) {
        state = BiQuadState{};
    }
    for (auto& state : mFilterStateR) {
        state = BiQuadState{};
    }

    // Initialize sag envelope coefficients
    float sagAttackSec = 0.001f;   // 1ms attack
    float sagReleaseSec = 0.1f;    // 100ms release
    mSagAttackCoeff = std::exp(-1.0f / (sagAttackSec * static_cast<float>(mSampleRate)));
    mSagReleaseCoeff = std::exp(-1.0f / (sagReleaseSec * static_cast<float>(mSampleRate)));

    updateFilterCoefficients();

    // Termina con reset() a proposito (WD-3.2): sus dos ParameterSmoother NO se
    // sembraban aca, asi que un AmpSimulator recien creado subia gain y master
    // desde cero durante el smoothing. Es el mismo defecto que tenia
    // DistortionEffect, y asi no puede volver: el estado inicial es POR
    // CONSTRUCCION el mismo que despues de un reset.
    reset();
    LOGI("AmpSimulator created");
}

void AmpSimulator::process(float* input, float* output, int numFrames) {
    // Load parameter targets atomically
    const float gainTarget = mGain.load(std::memory_order_relaxed) / 100.0f;
    const float presence = mPresence.load(std::memory_order_relaxed) / 100.0f;
    const float sag = mSag.load(std::memory_order_relaxed) / 100.0f;
    const float masterTarget = mMaster.load(std::memory_order_relaxed) / 100.0f;
    const AmpModel model = static_cast<AmpModel>(mAmpModel.load(std::memory_order_relaxed));

    for (int i = 0; i < numFrames; ++i) {
        const int idx = i * 2;

        // Smooth gain and master per-sample to prevent clicks
        float gain = mGainSmoother.process(gainTarget);
        float master = mMasterSmoother.process(masterTarget);

        // Process left channel
        float left = input[idx];
        left = processPreamp(left, model, gain);
        left = processToneStack(left, 0);  // Channel 0 = left
        left = processPowerAmp(left, presence, sag);

        // Process right channel
        float right = input[idx + 1];
        right = processPreamp(right, model, gain);
        right = processToneStack(right, 1);  // Channel 1 = right
        right = processPowerAmp(right, presence, sag);

        // Apply master volume
        output[idx] = left * master;
        output[idx + 1] = right * master;
    }
}

float AmpSimulator::processPreamp(float input, AmpModel model, float gain) {
    float output = input;

    // Model-dependent gain staging and saturation
    switch (model) {
        case AmpModel::CLEAN: {
            // Fender Twin style: linear with soft clipping
            float preGain = 1.0f + gain * 2.0f;  // 1x to 3x
            output = softClip(output * preGain);
            break;
        }

        case AmpModel::CRUNCH: {
            // Marshall JCM800 style: tube-like asymmetric saturation
            float preGain = 2.0f + gain * 8.0f;  // 2x to 10x
            output = tubeSimulation(output * preGain, 0.5f);
            break;
        }

        case AmpModel::HIGH_GAIN: {
            // Mesa Rectifier style: cascaded saturation stages
            float preGain = 5.0f + gain * 20.0f;  // 5x to 25x
            output = tubeSimulation(output * preGain * 0.5f, 0.8f);
            output = softClip(output * 2.0f);
            break;
        }

        case AmpModel::MODERN: {
            // 5150/6505 style: hard clipping + tube saturation
            float preGain = 8.0f + gain * 30.0f;  // 8x to 38x
            output = hardClip(output * preGain * 0.3f);
            output = tubeSimulation(output * 1.5f, 0.9f);
            break;
        }
    }

    return output;
}

float AmpSimulator::processToneStack(float input, int channel) {
    // Get correct filter state array
    auto& states = (channel == 0) ? mFilterStateL : mFilterStateR;

    float output = input;

    // Apply 4 BiQuad filters in series: Bass, Mid, Treble, Presence
    for (int i = 0; i < 4; ++i) {
        output = processBiQuad(output, states[i], mFilterCoeffs[i]);
    }

    return output;
}

float AmpSimulator::processPowerAmp(float input, float presence, float sag) {
    float output = input;

    // Presence boost (high frequency emphasis)
    float presenceBoost = 1.0f + presence * 0.3f;  // 1x to 1.3x
    output *= presenceBoost;

    // Sag simulation (power supply compression)
    if (sag > 0.01f) {
        float inputLevel = std::abs(output);

        // Envelope follower for sag
        if (inputLevel > mSagEnvelope) {
            mSagEnvelope = mSagAttackCoeff * mSagEnvelope + (1.0f - mSagAttackCoeff) * inputLevel;
        } else {
            mSagEnvelope = mSagReleaseCoeff * mSagEnvelope + (1.0f - mSagReleaseCoeff) * inputLevel;
        }

        // Apply sag compression
        float sagGain = 1.0f / (1.0f + sag * mSagEnvelope);
        output *= sagGain;
    }

    // Final soft clipping (power tube saturation)
    output = softClip(output);

    return output;
}

float AmpSimulator::softClip(float x) {
    // Tanh-based soft clipping
    return std::tanh(x);
}

float AmpSimulator::hardClip(float x) {
    // Digital hard clipping
    return std::clamp(x, -1.0f, 1.0f);
}

float AmpSimulator::tubeSimulation(float x, float drive) {
    // Asymmetric tube saturation
    // Positive half: faster saturation
    // Negative half: slower saturation (more headroom)
    if (x >= 0.0f) {
        return 1.0f - std::exp(-x * (1.0f + drive * 2.0f));
    } else {
        return -1.0f + std::exp(x * (1.0f + drive));
    }
}

float AmpSimulator::processBiQuad(float input, BiQuadState& state, const BiQuadCoeffs& coeffs) {
    // Direct Form II Transposed
    float output = coeffs.b0 * input + state.x1;
    state.x1 = coeffs.b1 * input - coeffs.a1 * output + state.x2;
    state.x2 = coeffs.b2 * input - coeffs.a2 * output;
    return output;
}

void AmpSimulator::updateFilterCoefficients() {
    const float bass = mBass.load(std::memory_order_relaxed);
    const float mid = mMid.load(std::memory_order_relaxed);
    const float treble = mTreble.load(std::memory_order_relaxed);
    const float presence = mPresence.load(std::memory_order_relaxed);
    const ToneStackType stackType = static_cast<ToneStackType>(mToneStack.load(std::memory_order_relaxed));

    // Convert 0-100 to -12 to +12 dB
    auto toDb = [](float value) {
        return (value - 50.0f) * 0.24f;  // 50 -> 0dB, 0 -> -12dB, 100 -> +12dB
    };

    float bassFreq, midFreq, trebleFreq;
    getToneStackFrequencies(stackType, bassFreq, midFreq, trebleFreq);

    // Calculate filter coefficients
    calculateLowShelf(0, bassFreq, toDb(bass));
    calculatePeaking(1, midFreq, toDb(mid), 1.0f);
    calculateHighShelf(2, trebleFreq, toDb(treble));
    calculateHighShelf(3, 4000.0f, toDb(presence) * 0.5f);  // Presence is gentler
}

void AmpSimulator::getToneStackFrequencies(ToneStackType type, float& bassFreq, float& midFreq, float& trebleFreq) {
    switch (type) {
        case ToneStackType::FENDER:
            bassFreq = 150.0f;
            midFreq = 600.0f;
            trebleFreq = 2500.0f;
            break;
        case ToneStackType::MARSHALL:
            bassFreq = 200.0f;
            midFreq = 1000.0f;
            trebleFreq = 3500.0f;
            break;
        case ToneStackType::VOX:
            bassFreq = 180.0f;
            midFreq = 900.0f;
            trebleFreq = 4000.0f;
            break;
        case ToneStackType::MESA:
            bassFreq = 100.0f;
            midFreq = 800.0f;
            trebleFreq = 3000.0f;
            break;
    }
}

void AmpSimulator::calculateLowShelf(int index, float freq, float gainDb, float q) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * static_cast<float>(M_PI) * freq / static_cast<float>(mSampleRate);
    float cosW0 = std::cos(w0);
    float sinW0 = std::sin(w0);
    float alpha = sinW0 / (2.0f * q);
    float sqrtA = std::sqrt(A);

    float a0 = (A + 1.0f) + (A - 1.0f) * cosW0 + 2.0f * sqrtA * alpha;
    mFilterCoeffs[index].b0 = (A * ((A + 1.0f) - (A - 1.0f) * cosW0 + 2.0f * sqrtA * alpha)) / a0;
    mFilterCoeffs[index].b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0)) / a0;
    mFilterCoeffs[index].b2 = (A * ((A + 1.0f) - (A - 1.0f) * cosW0 - 2.0f * sqrtA * alpha)) / a0;
    mFilterCoeffs[index].a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0)) / a0;
    mFilterCoeffs[index].a2 = ((A + 1.0f) + (A - 1.0f) * cosW0 - 2.0f * sqrtA * alpha) / a0;
}

void AmpSimulator::calculatePeaking(int index, float freq, float gainDb, float q) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * static_cast<float>(M_PI) * freq / static_cast<float>(mSampleRate);
    float cosW0 = std::cos(w0);
    float sinW0 = std::sin(w0);
    float alpha = sinW0 / (2.0f * q);

    float a0 = 1.0f + alpha / A;
    mFilterCoeffs[index].b0 = (1.0f + alpha * A) / a0;
    mFilterCoeffs[index].b1 = (-2.0f * cosW0) / a0;
    mFilterCoeffs[index].b2 = (1.0f - alpha * A) / a0;
    mFilterCoeffs[index].a1 = (-2.0f * cosW0) / a0;
    mFilterCoeffs[index].a2 = (1.0f - alpha / A) / a0;
}

void AmpSimulator::calculateHighShelf(int index, float freq, float gainDb, float q) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * static_cast<float>(M_PI) * freq / static_cast<float>(mSampleRate);
    float cosW0 = std::cos(w0);
    float sinW0 = std::sin(w0);
    float alpha = sinW0 / (2.0f * q);
    float sqrtA = std::sqrt(A);

    float a0 = (A + 1.0f) - (A - 1.0f) * cosW0 + 2.0f * sqrtA * alpha;
    mFilterCoeffs[index].b0 = (A * ((A + 1.0f) + (A - 1.0f) * cosW0 + 2.0f * sqrtA * alpha)) / a0;
    mFilterCoeffs[index].b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0)) / a0;
    mFilterCoeffs[index].b2 = (A * ((A + 1.0f) + (A - 1.0f) * cosW0 - 2.0f * sqrtA * alpha)) / a0;
    mFilterCoeffs[index].a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0)) / a0;
    mFilterCoeffs[index].a2 = ((A + 1.0f) - (A - 1.0f) * cosW0 - 2.0f * sqrtA * alpha) / a0;
}

void AmpSimulator::reset() {
    for (auto& state : mFilterStateL) state = BiQuadState{};
    for (auto& state : mFilterStateR) state = BiQuadState{};

    // La envolvente de sag es un seguidor: arrastra el nivel del audio viejo.
    mSagEnvelope = 0.0f;

    // Mismos targets que usa process(): gain y master vienen en 0..100.
    mGainSmoother.reset(mGain.load(std::memory_order_relaxed) / 100.0f);
    mMasterSmoother.reset(mMaster.load(std::memory_order_relaxed) / 100.0f);
}

void AmpSimulator::setParam(int paramId, float value) {
    switch (paramId) {
        case GAIN:
            mGain.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        case BASS:
            mBass.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        case MID:
            mMid.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        case TREBLE:
            mTreble.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        case PRESENCE:
            mPresence.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        case MASTER:
            mMaster.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        case SAG:
            mSag.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        case AMP_MODEL:
            mAmpModel.store(static_cast<int>(std::clamp(value, 0.0f, 3.0f)), std::memory_order_relaxed);
            break;
        case TONESTACK:
            mToneStack.store(static_cast<int>(std::clamp(value, 0.0f, 3.0f)), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        default:
            break;
    }
}

float AmpSimulator::getParam(int paramId) {
    switch (paramId) {
        case GAIN: return mGain.load(std::memory_order_relaxed);
        case BASS: return mBass.load(std::memory_order_relaxed);
        case MID: return mMid.load(std::memory_order_relaxed);
        case TREBLE: return mTreble.load(std::memory_order_relaxed);
        case PRESENCE: return mPresence.load(std::memory_order_relaxed);
        case MASTER: return mMaster.load(std::memory_order_relaxed);
        case SAG: return mSag.load(std::memory_order_relaxed);
        case AMP_MODEL: return static_cast<float>(mAmpModel.load(std::memory_order_relaxed));
        case TONESTACK: return static_cast<float>(mToneStack.load(std::memory_order_relaxed));
        default: return 0.0f;
    }
}

void AmpSimulator::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;

    // Update parameter smoothers
    float sr = static_cast<float>(sampleRate);
    mGainSmoother.setSmoothingTime(10.0f, sr);
    mMasterSmoother.setSmoothingTime(10.0f, sr);

    // Update sag envelope coefficients
    float sagAttackSec = 0.001f;
    float sagReleaseSec = 0.1f;
    mSagAttackCoeff = std::exp(-1.0f / (sagAttackSec * sr));
    mSagReleaseCoeff = std::exp(-1.0f / (sagReleaseSec * sr));

    updateFilterCoefficients();
    LOGI("Sample rate set to %d", sampleRate);
}
