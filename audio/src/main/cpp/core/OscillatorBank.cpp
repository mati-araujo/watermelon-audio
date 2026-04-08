#include "OscillatorBank.h"
#include "../platform/Logger.h"

#define LOG_TAG "OscillatorBank"

#ifdef NDEBUG
    #define LOGI(...) ((void)0)
    #define LOGW(...) ((void)0)
    #define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)
#else
    #define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
    #define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
    #define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)
#endif

OscillatorBank::OscillatorBank() {
    // Primary oscillators — order defines type ID
    mOscillators.push_back(std::make_unique<SineOscillator>());      // 0
    mOscillators.push_back(std::make_unique<SquareOscillator>());    // 1
    mOscillators.push_back(std::make_unique<SawtoothOscillator>());  // 2
    mOscillators.push_back(std::make_unique<TriangleOscillator>()); // 3
    mOscillators.push_back(std::make_unique<BandLimitedNoiseGenerator>()); // 4

    // Dual-touch oscillators — independent phase/smoothers
    mDualTouchOscillators.push_back(std::make_unique<SineOscillator>());
    mDualTouchOscillators.push_back(std::make_unique<SquareOscillator>());
    mDualTouchOscillators.push_back(std::make_unique<SawtoothOscillator>());
    mDualTouchOscillators.push_back(std::make_unique<TriangleOscillator>());
    mDualTouchOscillators.push_back(std::make_unique<BandLimitedNoiseGenerator>());

    LOGI("Primary oscillators: %zu, Dual-touch: %zu",
         mOscillators.size(), mDualTouchOscillators.size());

    // Modulators — index 0 = NONE (nullptr), 1-7 = active modulators
    mModulators.push_back(nullptr);  // NONE
    mModulators.push_back(std::make_unique<BurstModulator>());
    mModulators.push_back(std::make_unique<AMModulator>());
    mModulators.push_back(std::make_unique<FMModulator>());
    mModulators.push_back(std::make_unique<PWMModulator>());
    mModulators.push_back(std::make_unique<EnvelopeModulator>());
    mModulators.push_back(std::make_unique<RingModulator>());
    mModulators.push_back(std::make_unique<GateModulator>());

    LOGI("Modulators: %zu (including NONE)", mModulators.size());
}

void OscillatorBank::prepare(int sampleRate) {
    for (auto& osc : mOscillators) {
        osc->setSampleRate(sampleRate);
    }
    for (auto& osc : mDualTouchOscillators) {
        osc->setSampleRate(sampleRate);
    }
    for (auto& mod : mModulators) {
        if (mod != nullptr) {
            mod->setSampleRate(sampleRate);
        }
    }
    LOGI("Prepared at %d Hz", sampleRate);
}

void OscillatorBank::setOscillatorType(int typeId) {
    if (typeId >= 0 && typeId < static_cast<int>(mOscillators.size())) {
        mCurrentOscillatorIndex.store(typeId, std::memory_order_release);
    } else {
        LOGE("Invalid oscillator type: %d (valid range: 0-%zu)",
             typeId, mOscillators.size() - 1);
    }
}

void OscillatorBank::setModulatorType(int typeId) {
    if (typeId >= 0 && typeId < static_cast<int>(mModulators.size())) {
        mCurrentModulatorIndex.store(typeId, std::memory_order_release);
        mHasActiveModulator.store(typeId > 0, std::memory_order_release);

        // Reset the modulator when activated
        if (typeId > 0 && mModulators[typeId] != nullptr) {
            mModulators[typeId]->reset();
        }

        LOGI("Modulator changed to type %d (active=%s)",
             typeId, typeId > 0 ? "true" : "false");
    } else {
        LOGE("Invalid modulator type: %d (valid range: 0-%zu)",
             typeId, mModulators.size() - 1);
    }
}

void OscillatorBank::setModulatorParameter(int paramId, float value) {
    int modulatorIndex = mCurrentModulatorIndex.load(std::memory_order_acquire);

    if (modulatorIndex > 0 && modulatorIndex < static_cast<int>(mModulators.size())) {
        SignalModulator* modulator = mModulators[modulatorIndex].get();
        if (modulator != nullptr) {
            modulator->setParameter(paramId, value);
        }
    }
}

void OscillatorBank::renderPrimary(float* output, int numFrames) {
    int idx = mCurrentOscillatorIndex.load(std::memory_order_acquire);
    if (idx >= 0 && idx < static_cast<int>(mOscillators.size()) && mOscillators[idx]) {
        mOscillators[idx]->render(output, numFrames);
    }
}

void OscillatorBank::renderSecondary(float* output, int numFrames) {
    int idx = mCurrentOscillatorIndex.load(std::memory_order_acquire);
    if (idx >= 0 && idx < static_cast<int>(mDualTouchOscillators.size()) && mDualTouchOscillators[idx]) {
        mDualTouchOscillators[idx]->render(output, numFrames);
    }
}

void OscillatorBank::applyModulation(float* buffer, int numFrames) {
    if (!mHasActiveModulator.load(std::memory_order_acquire)) return;

    int idx = mCurrentModulatorIndex.load(std::memory_order_acquire);
    if (idx > 0 && idx < static_cast<int>(mModulators.size()) && mModulators[idx]) {
        mModulators[idx]->process(buffer, numFrames);
    }
}

void OscillatorBank::setFrequencyAndAmplitude(float freq, float amp) {
    int idx = mCurrentOscillatorIndex.load(std::memory_order_acquire);
    if (idx >= 0 && idx < static_cast<int>(mOscillators.size()) && mOscillators[idx]) {
        mOscillators[idx]->setParameters(freq, amp);
    }
}

void OscillatorBank::setSecondaryFrequencyAndAmplitude(float freq, float amp) {
    int idx = mCurrentOscillatorIndex.load(std::memory_order_acquire);
    if (idx >= 0 && idx < static_cast<int>(mDualTouchOscillators.size()) && mDualTouchOscillators[idx]) {
        mDualTouchOscillators[idx]->setParameters(freq, amp);
    }
}

void OscillatorBank::setAllPrimaryParams(float freq, float amp) {
    for (auto& osc : mOscillators) {
        osc->setParameters(freq, amp);
    }
}
