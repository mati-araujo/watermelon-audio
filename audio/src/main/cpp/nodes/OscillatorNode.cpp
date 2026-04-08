#include "OscillatorNode.h"
#include <cmath>
#include <algorithm>

OscillatorNode::OscillatorNode() {
    createOscillators();
    createModulators();
}

void OscillatorNode::createOscillators() {
    mOscillators.clear();
    mOscillators.push_back(std::make_unique<SineOscillator>());
    mOscillators.push_back(std::make_unique<SquareOscillator>());
    mOscillators.push_back(std::make_unique<SawtoothOscillator>());
    mOscillators.push_back(std::make_unique<TriangleOscillator>());
    mOscillators.push_back(std::make_unique<NoiseGenerator>());
    mOscillators.push_back(std::make_unique<BandLimitedNoiseGenerator>());
}

void OscillatorNode::createModulators() {
    mModulators.clear();
    // Index 0 = null (no modulation)
    mModulators.push_back(nullptr);
    mModulators.push_back(std::make_unique<BurstModulator>());
    mModulators.push_back(std::make_unique<AMModulator>());
    mModulators.push_back(std::make_unique<FMModulator>());
    mModulators.push_back(std::make_unique<PWMModulator>());
    mModulators.push_back(std::make_unique<EnvelopeModulator>());
    mModulators.push_back(std::make_unique<RingModulator>());
    mModulators.push_back(std::make_unique<GateModulator>());
}

void OscillatorNode::prepare(int sampleRate, int maxBlockSize) {
    AudioNode::prepare(sampleRate, maxBlockSize);

    // Configurar osciladores
    for (auto& osc : mOscillators) {
        if (osc) {
            osc->setSampleRate(sampleRate);
        }
    }

    // Configurar moduladores
    for (auto& mod : mModulators) {
        if (mod) {
            mod->setSampleRate(sampleRate);
        }
    }

    // Pre-alocar buffers temporales
    mTempBuffer.resize(maxBlockSize * 2, 0.0f);  // Estéreo interleaved
    mTouch1Buffer.resize(maxBlockSize * 2, 0.0f);
    mTouch2Buffer.resize(maxBlockSize * 2, 0.0f);
}

void OscillatorNode::reset() {
    AudioNode::reset();
    std::fill(mTempBuffer.begin(), mTempBuffer.end(), 0.0f);
    std::fill(mTouch1Buffer.begin(), mTouch1Buffer.end(), 0.0f);
    std::fill(mTouch2Buffer.begin(), mTouch2Buffer.end(), 0.0f);
}

void OscillatorNode::process(AudioBuffer& inputBuffer, int numFrames) {
    // Obtener parámetros actuales (lock-free)
    int oscIndex = mCurrentOscillatorIndex.load(std::memory_order_acquire);
    int modIndex = mCurrentModulatorIndex.load(std::memory_order_acquire);
    float freq = mFrequency.load(std::memory_order_acquire);
    float amp = mAmplitude.load(std::memory_order_acquire);
    bool dualTouch = mDualTouchMode.load(std::memory_order_acquire);

    // ========== SYNTH ENGINE DISPATCH (Phase 6) ==========
    int engineType = mCurrentEngineType.load(std::memory_order_acquire);
    if (engineType > 0 && engineType < MAX_ENGINES && mEngines[engineType]) {
        // Non-classic engine: delegate to SynthEngine
        SynthEngine* engine = mEngines[engineType];

        // Clear buffer before engine writes
        std::fill(mTempBuffer.begin(), mTempBuffer.begin() + numFrames * 2, 0.0f);

        engine->process(mTempBuffer.data(), numFrames, freq, amp);

        // Apply modulator if active (same as classic path)
        if (modIndex > 0 && modIndex < static_cast<int>(mModulators.size())) {
            SignalModulator* mod = mModulators[modIndex].get();
            if (mod) {
                mod->process(mTempBuffer.data(), numFrames);
            }
        }

        // Copy to output buffer (de-interleave)
        float* left = mBuffer.getWritePointer(0);
        float* right = mBuffer.getWritePointer(1);
        for (int i = 0; i < numFrames; ++i) {
            left[i] = mTempBuffer[i * 2];
            right[i] = mTempBuffer[i * 2 + 1];
        }
        return;
    }

    // ========== CLASSIC ENGINE (legacy, unchanged) ==========

    // Validar índice de oscilador
    if (oscIndex < 0 || oscIndex >= static_cast<int>(mOscillators.size())) {
        oscIndex = 0;
    }

    AudioSource* osc = mOscillators[oscIndex].get();
    if (!osc) return;

    if (!dualTouch) {
        // Modo single touch
        osc->setParameters(freq, amp);
        osc->render(mTempBuffer.data(), numFrames);

        // Aplicar modulador si está activo
        if (modIndex > 0 && modIndex < static_cast<int>(mModulators.size())) {
            SignalModulator* mod = mModulators[modIndex].get();
            if (mod) {
                mod->process(mTempBuffer.data(), numFrames);
            }
        }
    } else {
        // Modo dual touch
        float freq2 = mFrequency2.load(std::memory_order_acquire);
        float amp2 = mAmplitude2.load(std::memory_order_acquire);
        int oscIndex2 = mSecondaryOscillatorIndex.load(std::memory_order_acquire);

        if (oscIndex2 < 0 || oscIndex2 >= static_cast<int>(mOscillators.size())) {
            oscIndex2 = 0;
        }

        // Renderizar oscilador 1
        osc->setParameters(freq, amp);
        osc->render(mTouch1Buffer.data(), numFrames);

        // Renderizar oscilador 2
        AudioSource* osc2 = mOscillators[oscIndex2].get();
        if (osc2) {
            osc2->setParameters(freq2, amp2);
            osc2->render(mTouch2Buffer.data(), numFrames);
        }

        // Mezclar (promedio para evitar clipping)
        for (int i = 0; i < numFrames * 2; ++i) {
            mTempBuffer[i] = (mTouch1Buffer[i] + mTouch2Buffer[i]) * 0.5f;
        }

        // Aplicar modulador si está activo
        if (modIndex > 0 && modIndex < static_cast<int>(mModulators.size())) {
            SignalModulator* mod = mModulators[modIndex].get();
            if (mod) {
                mod->process(mTempBuffer.data(), numFrames);
            }
        }
    }

    // Copiar al buffer de salida del nodo (formato por canales separados)
    float* left = mBuffer.getWritePointer(0);
    float* right = mBuffer.getWritePointer(1);
    for (int i = 0; i < numFrames; ++i) {
        left[i] = mTempBuffer[i * 2];
        right[i] = mTempBuffer[i * 2 + 1];
    }
}

void OscillatorNode::updateXY(float x, float y) {
    float freq = mapXToFrequency(x);
    float amp = mapYToAmplitude(y);
    mFrequency.store(freq, std::memory_order_release);
    mAmplitude.store(amp, std::memory_order_release);
}

void OscillatorNode::setFrequencyAndAmplitude(float freq, float amp) {
    mFrequency.store(freq, std::memory_order_release);
    mAmplitude.store(amp, std::memory_order_release);
}

void OscillatorNode::setOscillatorType(int type) {
    if (type >= 0 && type < static_cast<int>(mOscillators.size())) {
        mCurrentOscillatorIndex.store(type, std::memory_order_release);
    }
}

int OscillatorNode::getOscillatorType() const {
    return mCurrentOscillatorIndex.load(std::memory_order_acquire);
}

void OscillatorNode::setModulatorType(int type) {
    if (type >= 0 && type < static_cast<int>(mModulators.size())) {
        mCurrentModulatorIndex.store(type, std::memory_order_release);
        // Reset del modulador al activarlo
        if (type > 0 && mModulators[type]) {
            mModulators[type]->reset();
        }
    }
}

int OscillatorNode::getModulatorType() const {
    return mCurrentModulatorIndex.load(std::memory_order_acquire);
}

void OscillatorNode::setModulatorParameter(int paramId, float value) {
    int modIndex = mCurrentModulatorIndex.load(std::memory_order_acquire);
    if (modIndex > 0 && modIndex < static_cast<int>(mModulators.size())) {
        SignalModulator* mod = mModulators[modIndex].get();
        if (mod) {
            mod->setParameter(paramId, value);
        }
    }
}

void OscillatorNode::setDualTouchMode(bool enabled) {
    mDualTouchMode.store(enabled, std::memory_order_release);
}

bool OscillatorNode::getDualTouchMode() const {
    return mDualTouchMode.load(std::memory_order_acquire);
}

void OscillatorNode::updateDualTouch(float x1, float y1, float freq1, float amp1,
                                     float x2, float y2, float freq2, float amp2) {
    // Touch 1: usar frecuencia directa si se proporciona, sino mapear desde XY
    if (freq1 > 0.0f) {
        mFrequency.store(freq1, std::memory_order_release);
        mAmplitude.store(amp1, std::memory_order_release);
    } else {
        mFrequency.store(mapXToFrequency(x1), std::memory_order_release);
        mAmplitude.store(mapYToAmplitude(y1), std::memory_order_release);
    }

    // Touch 2
    if (freq2 > 0.0f) {
        mFrequency2.store(freq2, std::memory_order_release);
        mAmplitude2.store(amp2, std::memory_order_release);
    } else {
        mFrequency2.store(mapXToFrequency(x2), std::memory_order_release);
        mAmplitude2.store(mapYToAmplitude(y2), std::memory_order_release);
    }
}

void OscillatorNode::setSecondaryOscillatorType(int type) {
    if (type >= 0 && type < static_cast<int>(mOscillators.size())) {
        mSecondaryOscillatorIndex.store(type, std::memory_order_release);
    }
}

int OscillatorNode::getSecondaryOscillatorType() const {
    return mSecondaryOscillatorIndex.load(std::memory_order_acquire);
}

// ========== Synth Engine (Phase 6) ==========

void OscillatorNode::setEngineType(int type) {
    if (type >= 0 && type < MAX_ENGINES) {
        mCurrentEngineType.store(type, std::memory_order_release);
    }
}

int OscillatorNode::getEngineType() const {
    return mCurrentEngineType.load(std::memory_order_acquire);
}

void OscillatorNode::setEngineParameter(int paramId, float value) {
    int engineType = mCurrentEngineType.load(std::memory_order_acquire);
    if (engineType > 0 && engineType < MAX_ENGINES && mEngines[engineType]) {
        mEngines[engineType]->setParameter(paramId, value);
    }
}

void OscillatorNode::registerEngine(int type, SynthEngine* engine) {
    if (type > 0 && type < MAX_ENGINES) {
        mEngines[type] = engine;
    }
}

void OscillatorNode::setFrequencyRange(float minHz, float maxHz) {
    if (!std::isfinite(minHz) || !std::isfinite(maxHz)) return;
    minHz = std::clamp(minHz, 8.0f, 20000.0f);
    maxHz = std::clamp(maxHz, 8.0f, 20000.0f);
    if (maxHz <= minHz) maxHz = minHz + 1.0f;
    // Store max first so a concurrent read never sees an inverted range
    mMaxFreq.store(maxHz, std::memory_order_release);
    mMinFreq.store(minHz, std::memory_order_release);
}

float OscillatorNode::mapXToFrequency(float x) const {
    float minF = mMinFreq.load(std::memory_order_relaxed);
    float maxF = mMaxFreq.load(std::memory_order_relaxed);
    if (maxF <= minF) maxF = minF + 1.0f; // safety for transient state
    float clampedX = std::clamp(x, 0.0f, 1.0f);
    return minF * std::pow(maxF / minF, clampedX);
}

float OscillatorNode::mapYToAmplitude(float y) const {
    // Mapeo lineal de 0-1
    return std::clamp(y, 0.0f, 1.0f);
}
