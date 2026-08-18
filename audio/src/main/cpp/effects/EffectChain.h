#ifndef EFFECTCHAIN_H
#define EFFECTCHAIN_H

#include "Effect.h"
#include "EffectTypes.h"
#include "EffectRegistry.h"
#include "../dsp/ParameterSmoother.h"
#include "../platform/RtCounter.h"
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
/**
 * @struct BranchDelay
 * @brief Linea de retardo entera para alinear una rama contra la mas lenta (WD-3.1).
 *
 * Compensacion de latencia: cuando dos ramas de un modo paralelo se suman, la
 * que tiene menos latencia hay que retrasarla la diferencia. Sin eso la suma es
 * un filtro peine — con DECI_HPF al maximo de reduccion son 479 samples, cuyo
 * primer notch cae en 50 Hz.
 *
 * Buffer fijo, alocado una vez. `process()` es RT-safe.
 */
struct BranchDelay {
    /// Tope de compensacion, en frames. 512 cubre el peor caso actual
    /// (DECI_HPF a 100 Hz de target: 479 samples) con margen. Una rama que pida
    /// mas se acota y se cuenta, en vez de alocar en el thread de audio.
    static constexpr int MAX_DELAY_FRAMES = 512;

    std::vector<float> buffer;  // interleaved estereo
    int writePos = 0;

    void prepare() {
        buffer.assign(static_cast<size_t>(MAX_DELAY_FRAMES) * 2, 0.0f);
        writePos = 0;
    }

    void clear() {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
    }

    /**
     * @brief Retrasa `io` en `delayFrames` frames, in-place. RT-safe.
     * @return true si se aplico el retardo pedido; false si hubo que acotarlo.
     */
    bool process(float* io, int numFrames, int delayFrames);
};

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
     * @brief Atomically clear ALL effects from the chain.
     *
     * Fast equivalent of calling removeEffect() in a loop. Single snapshot
     * swap + single 20ms grace sleep covering the entire batch (vs. one
     * sleep per effect with the per-effect API). Scene-load fast path.
     *
     * Thread-safe: Uses chainMutex internally.
     */
    void clearAllEffects();

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

    /**
     * @brief Clear all DSP state inside the chain and every effect.
     *
     * Zeros the chain-owned scratch/feedback/crossfade buffers AND calls
     * Effect::reset() on each effect in the active snapshot. Used when
     * the audio context changes in a way that would let stale state
     * bleed through — notably the chaos_pad → input_fx mode transition,
     * where a reverb tail cooked by synth audio would otherwise leak
     * into the first blocks of mic processing as a loud burst.
     *
     * RT-SAFE: zero-fills in place, no allocations, no locks. Must be
     * called from the audio thread (AudioEngine::onAudioReady dispatches
     * it via an atomic pending flag). Effect-internal state is owned by
     * the audio thread, so this is race-free with respect to process().
     */
    void reset();

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
     * @brief Set a global bypass over the full effect chain.
     *
     * This is independent from per-effect bypass flags. It behaves like a
     * performance "all FX bypass" pedal: individual bypass states remain
     * unchanged and are heard again when the global bypass is disabled.
     */
    void setGlobalBypass(bool bypass);

    /**
     * @brief Get requested global bypass state for the full effect chain.
     */
    bool getGlobalBypass() const;

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
     * @brief Latencia total que la cadena le agrega a la senal directa (WD-3.1).
     *
     * Suma de los efectos activos en modo SERIAL. En los modos que suman ramas
     * es el MAXIMO de las ramas, porque processWithMode() alinea la mas corta
     * contra la mas larga — que es de lo que se trata la compensacion.
     *
     * Lock-free: lee el snapshot activo. Llamable desde el thread de control.
     */
    int getLatencySamples() const;

    /// Latencia declarada por el efecto en `index`, o 0 si el indice no existe.
    int getEffectLatencySamples(size_t index) const;

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
     *
     * LOCK-FREE (WD-1.6). Lee un atomico que publica updateSnapshot() bajo
     * `chainMutex`. Antes tomaba el mutex, y como los cuatro setters del
     * vocoder lo llaman desde el thread de audio, eso ponia al callback a
     * esperar al thread que agrega efectos — que aloca con el lock tomado.
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

    // WD-1.6 — indice del vocoder, publicado por updateSnapshot() junto con el
    // snapshot. -1 = no hay vocoder en la cadena.
    std::atomic<int> mVocoderIndex{-1};

    // WD-1.1 — contadores que reemplazan a los logs que vivian en process().
    wma::RtCounter mNonFiniteBlocks;    ///< bloques con NaN/Inf saneados
    wma::RtCounter mOverflowBlocks;     ///< numFrames excedio el scratch pre-alocado
    wma::RtCounter mSilentOutputBlocks; ///< entrada con senal y salida en silencio
    wma::RtCounter mLatencyClampedBlocks; ///< una rama pidio mas compensacion que MAX_DELAY_FRAMES

    // WD-3.1 — compensacion de latencia entre ramas.
    //
    // Una linea por slot de efecto (los modos que suman ponen un efecto por
    // rama) mas dos para las ramas de SPLIT_2X2, que son rangos seriales.
    // Se alocan en el constructor: nunca se redimensionan en el callback.
    static constexpr size_t SPLIT_BRANCH_A = MAX_EFFECTS;
    static constexpr size_t SPLIT_BRANCH_B = MAX_EFFECTS + 1;
    std::array<BranchDelay, MAX_EFFECTS + 2> mBranchDelays;

    // Latencia de cada efecto y el maximo, RELEIDAS EN CADA BLOQUE desde
    // process(), no publicadas por updateSnapshot().
    //
    // Publicarlas en el snapshot era lo natural y estaba MAL: updateSnapshot()
    // corre solo en cambios ESTRUCTURALES (add/remove/reorder), y hay efectos
    // cuya latencia depende de un PARAMETRO — DECI_HPF va de 0 a 479 samples
    // segun su target de sample rate. Con las latencias congeladas al momento
    // de agregar el efecto, mover ese parametro dejaba la compensacion
    // desalineada sin que nada lo notara. Lo encontro el test de esta misma
    // tanda.
    //
    // Releerlas por bloque son 12 llamadas virtuales cada ~2,7 ms: despreciable,
    // y no puede quedar stale. Sin atomicos: las escribe y las lee el MISMO
    // thread de audio.
    std::array<int, MAX_EFFECTS> mRtEffectLatency{};
    int mRtMaxLatency = 0;
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
    // No mDepthValue here on purpose: the depth axis is driven end-to-end by the
    // normalizedValue argument of applyAutomation(axis=2), like X and Y. The old
    // atomic was a dead store across all four layers (removed 2026-07-27).

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
    ParameterSmoother mGlobalBypassSmooth;
    std::atomic<float> mGlobalBypassTarget{0.0f};

    /**
     * @brief Actualiza el snapshot activo después de cambios estructurales
     * IMPORTANTE: Debe llamarse con chainMutex locked
     */
    void updateSnapshot();

    // ========== ROUTING PROCESS STRATEGIES (RT-safe) ==========

    /**
     * @brief Alinea una rama contra la mas lenta de la cadena (WD-3.1).
     *
     * Retrasa `branch` en (maxLatencia - latenciaDeEstaRama) frames. Si la rama
     * ya es la mas lenta el retardo es cero y no se toca nada.
     *
     * RT-safe. `slot` indexa mBranchDelays: los indices [0, MAX_EFFECTS) son
     * los slots de efecto, SPLIT_BRANCH_A/B las dos ramas de SPLIT_2X2.
     */
    void compensateBranch(size_t slot, int branchLatency, float* branch, int numFrames);

    /** @brief El vocoder activo leido del snapshot, o nullptr. Lock-free (WD-1.6). */
    Effect* vocoderFromSnapshot() const;

    /**
     * @brief Procesa un efecto con suavizado de bypass y saneo de NaN/Inf.
     *
     * NO aplica ganancia. El auto-gain por efecto y por bloque que habia aca
     * salio en WD-3.3 — ver la nota en el .cpp. La proteccion de nivel es una
     * sola, al final, en `OutputStage`.
     */
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
