#include "CabinetSimulator.h"
#include <algorithm>
#include <cmath>
#include "../platform/Logger.h"

#define LOG_TAG "CabinetSimulator"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

CabinetSimulator::CabinetSimulator() {
    // Initialize buffers to zero
    reset();

    updateFilterCoefficients();

    // Load default IR
    loadIR(static_cast<BuiltInIRs::CabinetType>(mCabinetType.load(std::memory_order_relaxed)));

    LOGI("CabinetSimulator created");
}

void CabinetSimulator::loadIR(BuiltInIRs::CabinetType type) {
    std::lock_guard<std::mutex> lock(mIRMutex);

    const float* irData = BuiltInIRs::getIRData(type);
    if (irData == nullptr) {
        LOGW("No IR data for cabinet type %d, using bypass", static_cast<int>(type));
        mIRReady.store(false, std::memory_order_release);
        return;
    }

    int activeIndex = mActiveIRBuffer.load(std::memory_order_acquire);
    int inactiveIndex = 1 - activeIndex;
    auto& targetIR = mIRBuffers[static_cast<size_t>(inactiveIndex)];

    float absSum = 0.0f;
    for (size_t i = 0; i < IR_LENGTH; ++i) {
        absSum += std::abs(irData[i]);
    }

    // The built-in IRs are peak-normalized and have high absolute sums. Normalize
    // to conservative unity area so switching from 16 taps to 512 taps does not
    // create a large level jump in high-gain guitar chains.
    float norm = (absSum > 0.001f) ? (1.0f / absSum) : 1.0f;
    for (size_t i = 0; i < IR_LENGTH; ++i) {
        targetIR[i] = irData[i] * norm;
    }

    mActiveIRBuffer.store(inactiveIndex, std::memory_order_release);
    mIRReady.store(true, std::memory_order_release);

    LOGI("IR loaded for cabinet type %d (%s)", static_cast<int>(type),
         BuiltInIRs::getCabinetName(type));
}

void CabinetSimulator::process(float* input, float* output, int numFrames) {
    const float mix = mMix.load(std::memory_order_relaxed) / 100.0f;
    const bool irReady = mIRReady.load(std::memory_order_acquire);

    // If no IR loaded or mix is 0, bypass
    if (!irReady || mix < 0.001f) {
        std::copy(input, input + numFrames * 2, output);
        return;
    }

    int irIndex = mActiveIRBuffer.load(std::memory_order_acquire);
    const auto& ir = mIRBuffers[static_cast<size_t>(irIndex)];

    for (int i = 0; i < numFrames; ++i) {
        const int idx = i * 2;
        float dryL = input[idx];
        float dryR = input[idx + 1];

        // Apply filters to the current sample
        float wetSampleL = applyLowCut(dryL, mLowCutStateL);
        wetSampleL = applyHighCut(wetSampleL, mHighCutStateL);

        float wetSampleR = applyLowCut(dryR, mLowCutStateR);
        wetSampleR = applyHighCut(wetSampleR, mHighCutStateR);

        wetSampleL = processFirSample(wetSampleL, mDelayLineL, ir);
        wetSampleR = processFirSample(wetSampleR, mDelayLineR, ir);
        mDelayPos = (mDelayPos + 1) % IR_LENGTH;

        // Mix dry and wet
        output[idx] = dryL * (1.0f - mix) + wetSampleL * mix;
        output[idx + 1] = dryR * (1.0f - mix) + wetSampleR * mix;
    }
}

void CabinetSimulator::updateFilterCoefficients() {
    float lowCutFreq = mLowCut.load(std::memory_order_relaxed);
    float highCutFreq = mHighCut.load(std::memory_order_relaxed);

    // One-pole filter coefficients
    // High-pass (low cut): y[n] = (1-a) * x[n] + a * y[n-1]
    mLowCutCoeff = std::exp(-2.0f * static_cast<float>(M_PI) * lowCutFreq / static_cast<float>(mSampleRate));

    // Low-pass (high cut): y[n] = (1-a) * x[n] + a * y[n-1]
    mHighCutCoeff = std::exp(-2.0f * static_cast<float>(M_PI) * highCutFreq / static_cast<float>(mSampleRate));
}

float CabinetSimulator::applyLowCut(float input, float& state) {
    // High-pass filter (removes low frequencies)
    float output = input - state;
    state = mLowCutCoeff * state + (1.0f - mLowCutCoeff) * input;
    return output;
}

float CabinetSimulator::applyHighCut(float input, float& state) {
    // Low-pass filter (removes high frequencies)
    state = mHighCutCoeff * state + (1.0f - mHighCutCoeff) * input;
    return state;
}

float CabinetSimulator::processFirSample(float input,
                                         std::array<float, IR_LENGTH>& delayLine,
                                         const std::array<float, IR_LENGTH>& ir) {
    delayLine[mDelayPos] = input;

    float output = 0.0f;
    size_t readPos = mDelayPos;
    for (size_t tap = 0; tap < IR_LENGTH; ++tap) {
        output += delayLine[readPos] * ir[tap];
        readPos = (readPos == 0) ? (IR_LENGTH - 1) : (readPos - 1);
    }
    return output;
}

void CabinetSimulator::reset() {
    std::fill(mDelayLineL.begin(), mDelayLineL.end(), 0.0f);
    std::fill(mDelayLineR.begin(), mDelayLineR.end(), 0.0f);
    mDelayPos = 0;

    mLowCutStateL = 0.0f;
    mLowCutStateR = 0.0f;
    mHighCutStateL = 0.0f;
    mHighCutStateR = 0.0f;
}

void CabinetSimulator::setParam(int paramId, float value) {
    switch (paramId) {
        case CABINET: {
            int cabinetType = static_cast<int>(std::clamp(value, 0.0f, 6.0f));
            int prevType = mCabinetType.load(std::memory_order_relaxed);
            if (cabinetType != prevType) {
                mCabinetType.store(cabinetType, std::memory_order_relaxed);
                loadIR(static_cast<BuiltInIRs::CabinetType>(cabinetType));
            }
            break;
        }
        case MIX:
            mMix.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        case LOW_CUT:
            mLowCut.store(std::clamp(value, 20.0f, 500.0f), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        case HIGH_CUT:
            mHighCut.store(std::clamp(value, 2000.0f, 20000.0f), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        default:
            break;
    }
}

float CabinetSimulator::getParam(int paramId) {
    switch (paramId) {
        case CABINET: return static_cast<float>(mCabinetType.load(std::memory_order_relaxed));
        case MIX: return mMix.load(std::memory_order_relaxed);
        case LOW_CUT: return mLowCut.load(std::memory_order_relaxed);
        case HIGH_CUT: return mHighCut.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void CabinetSimulator::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    updateFilterCoefficients();

    // Reload IR to recalculate frequency domain representation if needed
    loadIR(static_cast<BuiltInIRs::CabinetType>(mCabinetType.load(std::memory_order_relaxed)));

    LOGI("Sample rate set to %d", sampleRate);
}
