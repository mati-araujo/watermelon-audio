#ifndef EFFECTCHAIN_H
#define EFFECTCHAIN_H

#include "Effect.h"
#include "EffectTypes.h"
#include "EffectRegistry.h"
#include "../dsp/ParameterSmoother.h"
#include <vector>
#include <memory>
#include <atomic>
#include <array>
#include <string>
#include <mutex>

struct Preset {
    std::string name;
    std::vector<std::vector<float>> params; // params[effectIndex][paramId]
};

/**
 * @struct AtomicMappingConfig
 * @brief RT-safe per-axis mapping configuration using individual atomics.
 *
 * Each field is independently atomic so the audio thread can read without locks.
 * Config changes are rare (user taps "Apply"), reads are at ~60Hz from applyAutomation().
 */
struct AtomicMappingConfig {
    std::atomic<int> effectIndex{-1};    ///< -1 = no mapping configured
    std::atomic<int> paramId{0};
    std::atomic<int> curve{0};           ///< MappingCurveType
    std::atomic<int> polarity{0};        ///< MappingPolarity
    std::atomic<float> mapMin{0.0f};
    std::atomic<float> mapMax{1.0f};
    std::atomic<bool> inverted{false};
};

/**
 * @struct EffectSnapshot
 * @brief Snapshot inmutable de la cadena de efectos para procesamiento RT-safe
 *
 * Este snapshot se crea cuando hay cambios estructurales y se lee atómicamente
 * desde el thread de audio sin necesidad de locks.
 */
struct EffectSnapshot {
    std::vector<Effect*> effects;      // Raw pointers (no ownership)
    std::vector<bool> bypassed;
    size_t size;

    EffectSnapshot() : size(0) {}
};

/**
 * @class EffectChain
 * @brief Cadena de efectos con procesamiento lock-free RT-safe
 *
 * Arquitectura:
 * - process() es completamente lock-free y RT-safe
 * - Usa atomic pointer swap para snapshots de la cadena
 * - Modificaciones estructurales usan mutex (solo en UI thread)
 * - Los efectos individuales son thread-safe (parámetros atómicos)
 */
class EffectChain {
public:
    EffectChain();
    ~EffectChain();

    // Prevenir copia (los efectos tienen estado mutable)
    EffectChain(const EffectChain&) = delete;
    EffectChain& operator=(const EffectChain&) = delete;

    /**
     * @brief Añade un efecto a la cadena
     * Thread-safe: Usa mutex interno
     * @return true si se añadió, false si cadena llena (max MAX_EFFECTS)
     */
    bool addEffect(EffectType type);

    /**
     * @brief Elimina un efecto de la cadena
     * Thread-safe: Usa mutex interno
     * IMPORTANTE: Espera un frame de audio antes de destruir el efecto
     */
    void removeEffect(size_t index);

    /**
     * @brief Reordena efectos en la cadena
     * Thread-safe: Usa mutex interno
     */
    void reorderEffects(size_t from, size_t to);

    /**
     * @brief Procesa audio a través de la cadena de efectos
     * RT-SAFE: Lock-free, no aloca memoria, no hace syscalls
     * Thread: Llamado desde audio callback de alta prioridad
     */
    void process(float* input, float* output, int numFrames);

    // ========== ROUTING MODE ==========

    /**
     * @brief Set routing mode with crossfade transition
     * Thread-safe: Atomic store, crossfade handled in process()
     */
    void setRoutingMode(RoutingMode mode);

    /**
     * @brief Set parallel mix balance (0=branchA, 1=branchB)
     * Lock-free: Atomic store
     */
    void setParallelMix(float mix);

    /**
     * @brief Set feedback amount (clamped to 0-0.95)
     * Lock-free: Atomic store
     */
    void setFeedbackAmount(float amount);

    RoutingMode getRoutingMode() const { return mRoutingMode.load(std::memory_order_relaxed); }

    /**
     * @brief Activa/desactiva bypass de un efecto
     * Lock-free: Usa atomic para el flag de bypass
     */
    void setBypass(size_t index, bool bypass);

    /**
     * @brief Obtiene estado de bypass
     * Lock-free: Lee atomic
     */
    bool getBypass(size_t index) const;

    /**
     * @brief Establece parámetro de efecto
     * Lock-free: Los efectos usan atómicos internamente
     */
    void setParameter(size_t index, int paramId, float value);

    /**
     * @brief Obtiene parámetro de efecto
     * Lock-free: Los efectos usan atómicos internamente
     */
    float getParameter(size_t index, int paramId) const;

    void savePreset(size_t presetId, const std::string& name);
    void loadPreset(size_t presetId);
    void setAutomationParameter(size_t effectIndex, int paramId, float xyValue);

    // ========== XY MAPPING CONFIG (Phase 4) ==========

    /**
     * @brief Configure mapping for an axis (X=0, Y=1, DEPTH=2)
     * Thread-safe: writes individual atomics
     */
    void setMappingConfig(int axis, int effectIndex, int paramId,
                          int curve, int polarity,
                          float mapMin, float mapMax, bool inverted);

    /**
     * @brief Clear mapping for an axis (sets effectIndex to -1)
     */
    void clearMappingConfig(int axis);

    /**
     * @brief Set depth axis value (0.0 to 1.0, from slider or dual-touch)
     * Lock-free: atomic store
     */
    void setDepthValue(float value) { mDepthValue.store(value, std::memory_order_relaxed); }

    /**
     * @brief Apply automation for an axis using stored mapping config
     * Lock-free: reads atomic config, calls setParam on target effect
     * @param axis 0=X, 1=Y, 2=DEPTH
     * @param normalizedValue 0.0 to 1.0 input from XY pad or depth slider
     */
    void applyAutomation(int axis, float normalizedValue);

    void setSampleRate(int sampleRate);

    /**
     * @brief Set global BPM (propagated to effects in process())
     * Lock-free: Uses atomic store
     */
    void setBpm(float bpm) { mBpm.store(bpm, std::memory_order_relaxed); }

    size_t getNumEffects() const;

    /**
     * @brief Gets the type of effect at given index
     * Thread-safe: Uses mutex internally
     * @param index Effect index in chain
     * @return Effect type, or FILTER if index invalid
     */
    EffectType getEffectType(size_t index) const;

    // ========== VOCODER-SPECIFIC METHODS ==========

    /**
     * @brief Find the index of the first vocoder in the chain
     * @return Index of vocoder, or -1 if not found
     */
    int findVocoderIndex() const;

    /**
     * @brief Set the modulator buffer for the vocoder (mic input)
     * @param buffer Mono audio buffer from microphone
     * @param numSamples Number of samples in buffer
     *
     * This allows passing external audio (e.g., microphone) to the vocoder
     * as the modulator signal, while the synth oscillator acts as carrier.
     */
    void setVocoderModulatorBuffer(const float* buffer, int numSamples);

    /**
     * @brief Update vocoder carrier frequency to match XY pad
     * @param frequency Frequency in Hz (20-2000Hz)
     *
     * Syncs the vocoder's internal carrier oscillator with the app's
     * oscillator frequency for responsive control.
     */
    void setVocoderCarrierFrequency(float frequency);

    /**
     * @brief Configure vocoder carrier source based on app mode
     * @param useInternalCarrier true = use internal synth, false = use input as carrier
     *
     * - OSCILLATOR mode: useInternalCarrier = false (input from oscillator is carrier)
     * - INPUT_FX mode: useInternalCarrier = true (internal synth, mic is modulator)
     * - MIX mode: useInternalCarrier = true (internal synth controlled by XY)
     */
    void setVocoderCarrierSource(bool useInternalCarrier);

    /**
     * @brief Configure vocoder modulator source
     * @param useExternalMod true = use external mic, false = self-vocoding
     *
     * Only relevant when carrierSource=0 (input as carrier).
     * When carrierSource=1, the input signal is automatically used as modulator.
     */
    void setVocoderModulatorSource(bool useExternalMod);

private:
    // Effect factory registry (Phase 1F)
    EffectRegistry mRegistry;

    // Efectos (ownership)
    std::vector<std::unique_ptr<Effect>> effects;
    std::vector<EffectType> effectTypes;

    // Bypass flags (thread-safe con mutex)
    std::vector<bool> bypassed;

    // Snapshot para RT thread (atomic pointer swap)
    std::atomic<EffectSnapshot*> mActiveSnapshot{nullptr};
    EffectSnapshot mSnapshot1;
    EffectSnapshot mSnapshot2;
    std::atomic<bool> mUsingSnapshot1{true};

    std::vector<Preset> presets;
    std::vector<float> tempBuffer1;  // Pre-allocated ping-pong buffers
    std::vector<float> tempBuffer2;  // for intermediate processing

    // Routing mode buffers (pre-allocated for parallel branches)
    std::vector<float> mBranchBufferA;   // rama A (stereo interleaved)
    std::vector<float> mBranchBufferB;   // rama B
    std::vector<float> mMixBuffer;       // accumulation buffer for parallel
    std::vector<float> mFeedbackBuffer;  // feedback path (persists between calls)

    mutable std::mutex chainMutex;  // ONLY for structural changes (add/remove), NOT for process()
    int mSampleRate = 48000;

    // Global BPM for tempo-synced effects
    std::atomic<float> mBpm{120.0f};
    float mLastBpm{120.0f};  // Last value propagated (avoids redundant calls)

    // XY Mapping configs (Phase 4)
    AtomicMappingConfig mXMapping;
    AtomicMappingConfig mYMapping;
    AtomicMappingConfig mDepthMapping;
    std::atomic<float> mDepthValue{0.0f};

    /**
     * @brief Get mapping config pointer for axis index
     * @return Pointer to config, or nullptr if axis invalid
     */
    AtomicMappingConfig* getMappingForAxis(int axis);
    const AtomicMappingConfig* getMappingForAxis(int axis) const;

    // Routing mode
    std::atomic<RoutingMode> mRoutingMode{RoutingMode::SERIAL};
    std::atomic<float> mParallelMix{0.5f};
    std::atomic<float> mFeedbackAmount{0.3f};

    // Crossfade state for routing mode transitions
    RoutingMode mCurrentProcessingMode{RoutingMode::SERIAL};
    RoutingMode mPendingRoutingMode{RoutingMode::SERIAL};
    int mCrossfadeSamples{0};      // total samples for crossfade (~30ms)
    int mCrossfadeCounter{0};      // remaining samples in crossfade
    std::vector<float> mCrossfadeBuffer;  // output from old mode during crossfade

    // Feedback safety: consecutive high-energy frame counter
    int mFeedbackHighEnergyFrames{0};

    // Bypass crossfade smoothers (pre-allocated, prevent clicks on bypass toggle)
    // 0.0 = fully active, 1.0 = fully bypassed
    static constexpr size_t MAX_BYPASS_SLOTS = MAX_EFFECTS;  // matches MAX_EFFECTS
    std::array<ParameterSmoother, MAX_BYPASS_SLOTS> mBypassSmooth;
    std::array<std::atomic<float>, MAX_BYPASS_SLOTS> mBypassTarget;

    /**
     * @brief Actualiza el snapshot activo después de cambios estructurales
     * IMPORTANTE: Debe llamarse con chainMutex locked
     */
    void updateSnapshot();

    // ========== ROUTING PROCESS STRATEGIES (RT-safe) ==========

    /** @brief Process single effect with bypass smoothing and auto-gain */
    void processOneEffect(Effect* effect, size_t slotIndex, bool isBypassed,
                          const float* input, float* output, int numFrames);

    /**
     * @brief Process a contiguous range of effects [startIdx, endIdx) in serial
     * RT-SAFE: Uses tempBuffer1/tempBuffer2 internally for ping-pong.
     * IMPORTANT: output must NOT alias tempBuffer1 or tempBuffer2.
     * Valid output targets: mBranchBufferA, mBranchBufferB, mMixBuffer, or external output.
     */
    void processSerialRange(EffectSnapshot* snapshot, size_t startIdx, size_t endIdx,
                            const float* input, float* output, int numFrames);

    void processSerial(EffectSnapshot* snapshot, const float* input, float* output, int numFrames);
    void processParallel(EffectSnapshot* snapshot, const float* input, float* output, int numFrames);
    void processSplit2x2(EffectSnapshot* snapshot, const float* input, float* output, int numFrames);
    void processSerialParallel(EffectSnapshot* snapshot, const float* input, float* output, int numFrames);
    void processParallelSerial(EffectSnapshot* snapshot, const float* input, float* output, int numFrames);
    void processFeedback(EffectSnapshot* snapshot, const float* input, float* output, int numFrames);

    /** @brief Route with the given mode (used for crossfade) */
    void processWithMode(RoutingMode mode, EffectSnapshot* snapshot,
                         const float* input, float* output, int numFrames);
};

#endif // EFFECTCHAIN_H