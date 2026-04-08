#include "EffectChainNode.h"
#include <algorithm>
#include <cstring>

EffectChainNode::EffectChainNode() = default;

void EffectChainNode::prepare(int sampleRate, int maxBlockSize) {
    AudioNode::prepare(sampleRate, maxBlockSize);

    // Configurar EffectChain
    mEffectChain.setSampleRate(sampleRate);

    // Pre-alocar buffers (interleaved estéreo)
    mInputBuffer.resize(maxBlockSize * 2, 0.0f);
    mOutputBuffer.resize(maxBlockSize * 2, 0.0f);
    mDryBuffer.resize(maxBlockSize * 2, 0.0f);
}

void EffectChainNode::reset() {
    AudioNode::reset();
    std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0f);
    std::fill(mOutputBuffer.begin(), mOutputBuffer.end(), 0.0f);
    std::fill(mDryBuffer.begin(), mDryBuffer.end(), 0.0f);
}

void EffectChainNode::process(AudioBuffer& inputBuffer, int numFrames) {
    // Convertir de formato por canales a interleaved para EffectChain
    const float* inLeft = inputBuffer.getReadPointer(0);
    const float* inRight = inputBuffer.getReadPointer(1);

    for (int i = 0; i < numFrames; ++i) {
        mInputBuffer[i * 2] = inLeft[i];
        mInputBuffer[i * 2 + 1] = inRight[i];
    }

    // Guardar dry signal para wet/dry mix
    float wetDry = mWetDryMix.load(std::memory_order_acquire);
    if (wetDry < 1.0f) {
        std::memcpy(mDryBuffer.data(), mInputBuffer.data(), numFrames * 2 * sizeof(float));
    }

    // Procesar a través del EffectChain
    mEffectChain.process(mInputBuffer.data(), mOutputBuffer.data(), numFrames);

    // Aplicar wet/dry mix si es necesario
    if (wetDry < 1.0f) {
        float dryGain = 1.0f - wetDry;
        for (int i = 0; i < numFrames * 2; ++i) {
            mOutputBuffer[i] = mOutputBuffer[i] * wetDry + mDryBuffer[i] * dryGain;
        }
    }

    // Convertir de interleaved a formato por canales para el buffer de salida
    float* outLeft = mBuffer.getWritePointer(0);
    float* outRight = mBuffer.getWritePointer(1);

    for (int i = 0; i < numFrames; ++i) {
        outLeft[i] = mOutputBuffer[i * 2];
        outRight[i] = mOutputBuffer[i * 2 + 1];
    }
}

bool EffectChainNode::addEffect(int effectType) {
    return mEffectChain.addEffect(static_cast<EffectType>(effectType));
}

void EffectChainNode::removeEffect(int index) {
    mEffectChain.removeEffect(static_cast<size_t>(index));
}

void EffectChainNode::clearEffects() {
    // Remover efectos de atrás hacia adelante
    while (mEffectChain.getNumEffects() > 0) {
        mEffectChain.removeEffect(mEffectChain.getNumEffects() - 1);
    }
}

void EffectChainNode::setEffectParameter(int effectIndex, int paramId, float value) {
    mEffectChain.setParameter(static_cast<size_t>(effectIndex), paramId, value);
}

float EffectChainNode::getEffectParameter(int effectIndex, int paramId) const {
    return mEffectChain.getParameter(static_cast<size_t>(effectIndex), paramId);
}

void EffectChainNode::setEffectBypassed(int effectIndex, bool bypassed) {
    mEffectChain.setBypass(static_cast<size_t>(effectIndex), bypassed);
}

bool EffectChainNode::isEffectBypassed(int effectIndex) const {
    return mEffectChain.getBypass(static_cast<size_t>(effectIndex));
}

void EffectChainNode::reorderEffect(int fromIndex, int toIndex) {
    mEffectChain.reorderEffects(static_cast<size_t>(fromIndex), static_cast<size_t>(toIndex));
}

void EffectChainNode::setWetDryMix(float wet) {
    mWetDryMix.store(std::clamp(wet, 0.0f, 1.0f), std::memory_order_release);
}

float EffectChainNode::getWetDryMix() const {
    return mWetDryMix.load(std::memory_order_acquire);
}

int EffectChainNode::getEffectCount() const {
    return static_cast<int>(mEffectChain.getNumEffects());
}
