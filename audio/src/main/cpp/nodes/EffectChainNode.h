#pragma once

#include "../core/graph/AudioNode.h"
#include "../effects/EffectChain.h"
#include <atomic>

/**
 * @class EffectChainNode
 * @brief Nodo de cadena de efectos para el Audio Graph
 *
 * Wrapper del EffectChain existente que lo integra en la arquitectura
 * del Audio Graph. Delega todas las operaciones de efectos al
 * EffectChain interno.
 */
class EffectChainNode : public AudioNode {
public:
    EffectChainNode();
    ~EffectChainNode() override = default;

    NodeType getType() const override { return NodeType::EFFECT_CHAIN; }
    const char* getName() const override { return "Effect Chain"; }

    void prepare(int sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(AudioBuffer& inputBuffer, int numFrames) override;

    // ========== Delegación al EffectChain existente ==========

    /**
     * @brief Agrega un efecto a la cadena
     * @param effectType Tipo de efecto (FILTER=0, REVERB=1, DELAY=2)
     * @return true si se agregó exitosamente
     */
    bool addEffect(int effectType);

    /**
     * @brief Remueve un efecto por índice
     */
    void removeEffect(int index);

    /**
     * @brief Limpia todos los efectos
     */
    void clearEffects();

    /**
     * @brief Establece parámetro de un efecto
     */
    void setEffectParameter(int effectIndex, int paramId, float value);

    /**
     * @brief Obtiene parámetro de un efecto
     */
    float getEffectParameter(int effectIndex, int paramId) const;

    /**
     * @brief Activa/desactiva bypass de un efecto
     */
    void setEffectBypassed(int effectIndex, bool bypassed);

    /**
     * @brief Verifica si un efecto está bypassed
     */
    bool isEffectBypassed(int effectIndex) const;

    /**
     * @brief Reordena un efecto en la cadena
     */
    void reorderEffect(int fromIndex, int toIndex);

    // ========== Wet/Dry global ==========

    void setWetDryMix(float wet);
    float getWetDryMix() const;

    // ========== Estado ==========

    int getEffectCount() const;

    // Acceso directo al EffectChain (para casos especiales)
    EffectChain& getEffectChain() { return mEffectChain; }
    const EffectChain& getEffectChain() const { return mEffectChain; }

private:
    EffectChain mEffectChain;
    std::atomic<float> mWetDryMix{1.0f};

    // Buffers para procesamiento
    std::vector<float> mInputBuffer;   // Interleaved para EffectChain
    std::vector<float> mOutputBuffer;  // Interleaved para EffectChain
    std::vector<float> mDryBuffer;     // Para wet/dry mix
};
