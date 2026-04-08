#pragma once

#include "../core/graph/AudioNode.h"
#include "../dsp/SoftClipper.h"
#include "../dsp/Dithering.h"
#include "../dsp/DCBlocker.h"
#include <atomic>
#include <vector>

/**
 * @class OutputNode
 * @brief Nodo de salida final para el Audio Graph
 *
 * Aplica el procesamiento de salida final:
 * - DC Blocking
 * - Soft Clipping (protección contra overshoots)
 * - Dithering (para conversión float→int16)
 * - Master Volume
 * - Fade In/Out
 * - Metering (peak/RMS)
 *
 * El buffer de salida final está en formato interleaved estéreo
 * listo para enviar a Oboe.
 */
class OutputNode : public AudioNode {
public:
    OutputNode();
    ~OutputNode() override = default;

    NodeType getType() const override { return NodeType::OUTPUT; }
    const char* getName() const override { return "Output"; }

    void prepare(int sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(AudioBuffer& inputBuffer, int numFrames) override;

    // ========== Master controls ==========

    void setMasterVolume(float volume);
    float getMasterVolume() const;

    void setMasterMute(bool mute);
    bool isMasterMuted() const;

    // ========== Limiter ==========

    void setLimiterEnabled(bool enabled);
    bool isLimiterEnabled() const;

    // ========== Metering (para UI) ==========

    float getPeakLevel(int channel) const;
    float getRMSLevel(int channel) const;

    // ========== Fade control ==========

    /**
     * @brief Inicia un fade in
     * @param durationMs Duración en milisegundos
     */
    void startFadeIn(float durationMs);

    /**
     * @brief Inicia un fade out
     * @param durationMs Duración en milisegundos
     */
    void startFadeOut(float durationMs);

    /**
     * @brief Verifica si hay un fade en progreso
     */
    bool isFading() const;

    /**
     * @brief Obtiene el progreso del fade (0.0 - 1.0)
     */
    float getFadeProgress() const;

    // ========== Output buffer final (para escribir a Oboe) ==========

    /**
     * @brief Obtiene el buffer de salida final (interleaved estéreo)
     */
    const float* getFinalOutputBuffer() const;

    /**
     * @brief Obtiene el número de frames del último proceso
     */
    int getFinalOutputNumFrames() const;

private:
    void processFade(float* buffer, int numFrames);
    void updateMeters(const float* buffer, int numFrames);

private:
    std::atomic<float> mMasterVolume{1.0f};
    std::atomic<bool> mMasterMute{false};
    std::atomic<bool> mLimiterEnabled{true};

    // DSP de salida
    SoftClipper mSoftClipper;
    StereoDitherer mDitherer;
    StereoDCBlocker mDCBlocker;

    // Fade
    enum class FadeState { NONE, FADING_IN, FADING_OUT };
    std::atomic<FadeState> mFadeState{FadeState::NONE};
    std::atomic<float> mFadePosition{1.0f};
    std::atomic<float> mFadeIncrement{0.0f};
    std::atomic<int> mFadeRemainingFrames{0};
    std::atomic<int> mFadeTotalFrames{0};

    // Metering (valores RMS calculados con media exponencial)
    std::atomic<float> mPeakL{0.0f};
    std::atomic<float> mPeakR{0.0f};
    std::atomic<float> mRmsL{0.0f};
    std::atomic<float> mRmsR{0.0f};
    float mRmsCoeff{0.0f};  // Coeficiente para media exponencial

    // Buffer de salida final (interleaved estéreo)
    std::vector<float> mFinalOutputBuffer;
    int mLastNumFrames{0};
};
