#pragma once

// Forward declaration — Oboe is only needed in AudioEngine.cpp (legacy path)
namespace oboe { class AudioStream; }

#include <algorithm>
#include <cmath>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "../backends/IAudioBackend.h"
#include "../effects/EffectChain.h"
#include "WaveformCapture.h"
#include "OutputStage.h"
#include "DualTouchManager.h"
#include "OscillatorBank.h"
#include "../nodes/MixerNode.h"
#include "../nodes/OscillatorNode.h"
#include "../nodes/EffectChainNode.h"
#include "../nodes/OutputNode.h"
#include "graph/AudioBuffer.h"
// AudioGraph forward-declared; full include in AudioEngine.cpp
#include "../voice/VoiceManager.h"
// TouchTriggerSource.h only needed in AudioEngine.cpp
#include "SynthEngineDispatcher.h"
#include "../sequencer/ArpSequencer.h"
#include "../looper/AudioLooper.h"
#include "../looper/Transport.h"
#include "../looper/PreRollRing.h"
#include "FadeController.h"
#include "ChordHarmony.h"

// Forward declarations (full includes in AudioEngine.cpp)
class InputNode;
class AudioGraph;

// Include for shared_ptr atomic operations
#include <memory>

/**
 * @brief Estados del ciclo de vida del motor de audio
 *
 * Estados válidos y transiciones:
 * Stopped -> Starting -> Running -> Stopping -> Stopped
 */
enum class EngineState {
    Stopped,   ///< Motor detenido, sin stream activo
    Starting,  ///< Iniciando stream, en transición
    Running,   ///< Stream activo, procesando audio
    Stopping   ///< Deteniendo stream, en transición
};

// DualTouchMixMode enum moved to DualTouchManager.h

/**
 * @brief Audio processing mode determined per-callback.
 *
 * Each mode maps to a dedicated render method in AudioEngine.
 */
enum class AudioProcessingMode {
    INPUT_FX,       ///< Input signal through effect chain (oscillator disabled)
    SOUNDFONT,      ///< SoundFont engine (tsf polyphony)
    VOICE_SYSTEM,   ///< Polyphonic voice manager
    SINGLE_TOUCH,   ///< Single oscillator/engine + arp + effects
    DUAL_TOUCH      ///< Two independent oscillators/engines
};

/**
 * @class AudioEngine
 * @brief Motor de audio principal con gestión robusta de ciclo de vida
 *
 * Implementa:
 * - Gestión de estado thread-safe con máquina de estados
 * - Sincronización correcta entre UI y audio threads
 * - Protección contra múltiples start/stop simultáneos
 * - Cleanup garantizado de recursos (RAII)
 * - Lock-free audio processing path
 * - Manejo de desconexión de hardware (Fase 2.1.4)
 * - Backend abstraction for USB audio support (Phase 1)
 *
 * Audio processing is now always routed through the IAudioCallback interface.
 * The legacy direct-Oboe path uses an internal OboeCallbackAdapter.
 */
class AudioEngine : public watermelon_audio::IAudioCallback {
public:
    AudioEngine();
    ~AudioEngine();

    // Prevenir copia y movimiento (singleton-like behavior)
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

    /**
     * @brief Inicia el motor de audio
     * @return true si se inició correctamente, false si ya estaba iniciado
     *
     * Thread-safe: Puede llamarse desde cualquier thread
     * Idempotente: Llamadas múltiples son seguras (solo la primera tiene efecto)
     */
    bool start(int fadeTimeMs = 10);

    /**
     * @brief Detiene el motor de audio
     *
     * Thread-safe: Puede llamarse desde cualquier thread
     * Blocking: Espera a que los callbacks de audio terminen antes de retornar
     * Idempotente: Llamadas múltiples son seguras
     */
    void stop();

    /**
     * @brief Establece el volumen maestro (0.0 - 1.0)
     * Lock-free: Seguro llamar desde cualquier thread
     */
    void setMasterVolume(float volume);

    /**
     * @brief Inicia el motor con fade in
     * @param fadeTimeMs Duración del fade in en milisegundos
     */
    bool startWithFade(int fadeTimeMs);

    /**
     * @brief Detiene el motor con fade out
     * @param fadeTimeMs Duración del fade out en milisegundos
     */
    void stopWithFade(int fadeTimeMs);

    /**
     * @brief Pausa el audio con fade out (mantiene el stream activo)
     * @param fadeTimeMs Duración del fade out en milisegundos
     */
    void pauseWithFade(int fadeTimeMs);

    /**
     * @brief Reanuda el audio con fade in
     * @param fadeTimeMs Duración del fade in en milisegundos
     */
    void resumeWithFade(int fadeTimeMs);

    /**
     * @brief Actualiza parámetros XY de los osciladores
     * Lock-free: Seguro llamar desde cualquier thread
     */
    void updateXY(float x, float y);

    /**
     * @brief Establece la frecuencia y amplitud directamente
     * @param frequency Frecuencia en Hz (20Hz - 20000Hz)
     * @param amplitude Amplitud (0.0 - 1.0)
     *
     * Este método permite establecer frecuencias exactas sin conversión XY,
     * lo cual es esencial para cuantización de escalas musicales.
     * Lock-free: Seguro llamar desde cualquier thread
     */
    void setFrequencyAndAmplitude(float frequency, float amplitude);

    /**
     * @brief Sets the dynamic frequency range for XY mapping (Phase 10A).
     * Lock-free: Safe to call from any thread.
     */
    void setFrequencyRange(float minHz, float maxHz);

    /**
     * @brief Cambia el tipo de oscilador activo
     * Lock-free: Seguro llamar desde cualquier thread
     */
    void setOscillatorType(int typeId);

    // ========== VOICE FILTER (Phase 6) ==========

    void setVoiceFilterEnabled(bool enabled);
    void setVoiceFilterCutoff(float hz);
    void setVoiceFilterResonance(float q);
    void setVoiceFilterMode(int mode);  // 0=LP, 1=HP, 2=BP

    // ========== SYNTH ENGINE SYSTEM (Phase 6) ==========

    /**
     * @brief Set the active synthesis engine type
     * @param engineType Engine ID (0=CLASSIC, 1=KS, 2=FM, 3=WT, 4=GRAIN, 5=SUPER, 6=SF)
     *
     * CLASSIC (0) uses the legacy AudioSource oscillators unchanged.
     * Other types delegate to the corresponding SynthEngine instance.
     * Lock-free: Safe to call from any thread.
     */
    void setEngineType(int engineType);

    /**
     * @brief Get the current engine type
     * @return Engine type ID
     */
    int getEngineType() const {
        return mEngineDispatcher.getEngineType();
    }

    /**
     * @brief Set a parameter on the current synth engine
     * @param paramId Parameter index (0 to MAX_ENGINE_PARAMS-1)
     * @param value Parameter value (typically 0.0-1.0)
     *
     * No-op for CLASSIC engine (classic oscillators have no engine params).
     * Lock-free: Safe to call from any thread.
     */
    void setEngineParameter(int paramId, float value);

    // ========== SOUNDFONT ENGINE (Phase 8) ==========

    /**
     * @brief Load a SoundFont from a memory buffer
     * @param data Raw .sf2 file data
     * @param size Size in bytes
     * @return true if loading succeeded
     *
     * NOT RT-safe — call from JNI/background thread.
     */
    bool loadSoundFont(const void* data, int size);

    /**
     * @brief Load a SoundFont from a file path using mmap (zero-copy)
     * @param path Absolute path to .sf2 file
     * @return true if loading succeeded
     */
    bool loadSoundFontFromPath(const char* path);

    /**
     * @brief Load a SoundFont from a sub-region of a file descriptor (mmap).
     * @param fd     Open, readable fd. Owned by the caller (not dup'd/closed here).
     * @param offset Byte offset of the SoundFont within the fd's file.
     * @param length Length of the SoundFont region, in bytes.
     * @return true if loading succeeded
     *
     * For bundled assets exposed as an AssetFileDescriptor (Play Asset
     * Delivery). Synchronous; the fd only needs to stay open for the call.
     */
    bool loadSoundFontFromFd(int fd, int64_t offset, int64_t length);

    /**
     * @brief Unload the current SoundFont
     */
    void unloadSoundFont();

    /**
     * @brief Set the active preset for SoundFont engine
     * @param presetIndex Preset index (0 to presetCount-1)
     */
    void setSoundFontPreset(int presetIndex);

    /**
     * @brief Get number of presets in loaded SoundFont
     * @return Preset count, or 0 if none loaded
     */
    int getSoundFontPresetCount() const;

    /**
     * @brief Get preset name by index
     * @return Name string, or nullptr
     */
    const char* getSoundFontPresetName(int presetIndex) const;

    /**
     * @brief Get the MIDI key range for a SoundFont preset (Phase 10B)
     * @param presetIndex Preset index
     * @param outMinKey Lowest MIDI key with samples
     * @param outMaxKey Highest MIDI key with samples
     * @return true if preset exists
     */
    bool getSoundFontPresetKeyRange(int presetIndex, int& outMinKey, int& outMaxKey) const;

    /**
     * @brief Get the SF2 bank + GM program for a SoundFont preset.
     * @param presetIndex Preset index
     * @param outBank SF2 bank (128 = GM percussion kit)
     * @param outProgram GM program number (0-127)
     * @return true if preset exists
     */
    bool getSoundFontPresetBankProgram(int presetIndex, int& outBank, int& outProgram) const;

    /**
     * @brief Check if a SoundFont is loaded
     */
    bool isSoundFontLoaded() const;

    /**
     * @brief Start/update a SoundFont note for a touch point (audio thread)
     * @param touchId Touch index (0 to 9)
     * @param midiNote MIDI note (0-127)
     * @param velocity Velocity (0.0-1.0)
     */
    void sfNoteOn(int touchId, int midiNote, float velocity);

    /**
     * @brief Release a SoundFont note for a touch point (audio thread)
     * @param touchId Touch index (0 to 9)
     */
    void sfNoteOff(int touchId);

    /**
     * @brief Release all SoundFont notes
     */
    void sfNoteOffAll();

    /**
     * @brief Release every active SoundFont touch except @p keepTouchId.
     *
     * Designed for single-touch XY drag flows where the UI must release
     * leftover dual-touch slots without paying one JNI call per slot.
     * Lock-free: enqueues a single event onto the SoundFontEngine queue;
     * the touch-state scan runs on the audio thread.
     */
    void sfNoteOffAllExcept(int keepTouchId);

    /**
     * @brief Cambia el tipo de modulador activo
     * @param typeId Tipo de modulador (0=NONE, 1=BURST, 2=AM, 3=FM, 4=PWM, 5=ENV, 6=RING, 7=GATE)
     * Lock-free: Seguro llamar desde cualquier thread
     */
    void setModulatorType(int typeId);

    /**
     * @brief Establece un parámetro del modulador activo
     * @param paramId Identificador del parámetro (específico del modulador)
     * @param value Valor del parámetro (típicamente 0.0 - 1.0)
     * Lock-free: Seguro llamar desde cualquier thread
     */
    void setModulatorParameter(int paramId, float value);

    /**
     * @brief Obtiene muestras de forma de onda para visualización
     * @param buffer Buffer de salida (debe ser no-null)
     * @param size Tamaño del buffer (máximo 1024)
     * @return Número de samples escritos (0 si buffer es null o size inválido)
     *
     * Thread-safe: Usa spinlock ligero para acceso al buffer circular
     */
    int getWaveformSamples(float* buffer, int size);

    // Effect chain access methods (thread-safe, delegan a EffectChain)
    // CRITICAL FIX: Each mutation must call incrementStateVersion() for StateSynchronizer to detect changes
    bool addEffect(EffectType type) {
        bool result = mEffectChain.addEffect(type);
        if (result) incrementStateVersion();
        return result;
    }
    void removeEffect(size_t index) {
        mEffectChain.removeEffect(index);
        incrementStateVersion();
    }
    void clearAllEffects() {
        mEffectChain.clearAllEffects();
        incrementStateVersion();
    }
    void reorderEffects(size_t from, size_t to) {
        mEffectChain.reorderEffects(from, to);
        incrementStateVersion();
    }
    void setBypass(size_t index, bool bypass) {
        mEffectChain.setBypass(index, bypass);
        incrementStateVersion();
    }
    bool getBypass(size_t index) const { return mEffectChain.getBypass(index); }
    void setEffectsBypass(bool bypass) {
        mEffectChain.setGlobalBypass(bypass);
        incrementStateVersion();
    }
    bool isEffectsBypassed() const { return mEffectChain.getGlobalBypass(); }
    void setParameter(size_t index, int paramId, float value) {
        mEffectChain.setParameter(index, paramId, value);
        incrementStateVersion();
    }

    /**
     * Apply many parameter updates with a single state-version bump.
     *
     * Scene-load fast path. Each update writes directly through the existing
     * lock-free atomic snapshot on EffectChain — individual params are
     * std::atomic, so audio-thread reads remain consistent. We bump the state
     * version exactly once at the end so the Kotlin-side StateSynchronizer
     * emits a single coherent post-batch state.
     *
     * Out-of-range effect indices are skipped silently.
     */
    void setParametersBatch(const int* effectIndices,
                            const int* paramIds,
                            const float* values,
                            size_t count) {
        if (count == 0 || effectIndices == nullptr || paramIds == nullptr || values == nullptr) {
            return;
        }
        const size_t chainSize = mEffectChain.getNumEffects();
        bool anyApplied = false;
        for (size_t i = 0; i < count; ++i) {
            const int idx = effectIndices[i];
            if (idx >= 0 && static_cast<size_t>(idx) < chainSize && std::isfinite(values[i])) {
                mEffectChain.setParameter(static_cast<size_t>(idx), paramIds[i], values[i]);
                anyApplied = true;
            }
        }
        if (anyApplied) incrementStateVersion();
    }

    float getParameter(size_t index, int paramId) const { return mEffectChain.getParameter(index, paramId); }
    void savePreset(size_t presetId, const std::string& name) { mEffectChain.savePreset(presetId, name); }
    void loadPreset(size_t presetId) {
        mEffectChain.loadPreset(presetId);
        incrementStateVersion();
    }
    void setAutomationParameter(size_t effectIndex, int paramId, float xyValue) {
        mEffectChain.setAutomationParameter(effectIndex, paramId, xyValue);
        incrementStateVersion();
    }

    // ========== XY MAPPING CONFIG (Phase 4) ==========

    void setMappingConfig(int axis, int effectIndex, int paramId,
                          int curve, int polarity,
                          float mapMin, float mapMax, bool inverted) {
        mEffectChain.setMappingConfig(axis, effectIndex, paramId, curve, polarity, mapMin, mapMax, inverted);
    }
    void clearMappingConfig(int axis) {
        mEffectChain.clearMappingConfig(axis);
    }
    void applyAutomation(int axis, float normalizedValue) {
        mEffectChain.applyAutomation(axis, normalizedValue);
    }

    size_t getNumEffects() const { return mEffectChain.getNumEffects(); }
    EffectType getEffectType(size_t index) const { return mEffectChain.getEffectType(index); }
    bool isBypassed(size_t index) const { return mEffectChain.getBypass(index); }

    /**
     * @brief Ask the audio thread to clear all effect chain DSP state
     *        on its next onAudioReady callback.
     *
     * Lock-free request: sets an atomic flag read by the audio thread.
     * The actual EffectChain::reset() call happens on the audio thread
     * (race-free with process()). Call this from the UI / JNI thread
     * when the audio context changes in a way that would let stale
     * effect state bleed through — notably the chaos_pad → input_fx
     * mode transition, where a reverb tail cooked by loud synth audio
     * leaks into the first blocks of mic processing as a loud burst.
     *
     * Idempotent: multiple requests before the audio thread services
     * the flag collapse into a single reset.
     */
    void requestResetEffectChain() {
        mResetEffectChainPending.store(true, std::memory_order_release);
    }

    // ========== EFFECT ROUTING MODE ==========

    void setRoutingMode(RoutingMode mode) {
        mEffectChain.setRoutingMode(mode);
    }

    void setParallelMix(float mix) {
        mEffectChain.setParallelMix(mix);
    }

    void setFeedbackAmount(float amount) {
        mEffectChain.setFeedbackAmount(amount);
    }

    int getRoutingMode() const {
        return static_cast<int>(mEffectChain.getRoutingMode());
    }

    // ========== GLOBAL BPM (KORG FX) ==========

    /**
     * @brief Set global BPM for tempo-synced effects
     * @param bpm Beats per minute (20-300)
     * Lock-free: Safe to call from any thread
     */
    void setBpm(float bpm) {
        bpm = std::clamp(bpm, 20.0f, 300.0f);
        mBpm.store(bpm, std::memory_order_relaxed);
        mEffectChain.setBpm(bpm);
        mTransport.setBpm(bpm);
        incrementStateVersion();
    }

    /**
     * @brief Get current global BPM
     */
    float getBpm() const {
        return mBpm.load(std::memory_order_relaxed);
    }

    // ========== ARPEGGIATOR (Phase 7) ==========

    /** Get the ArpSequencer for direct configuration from JNI */
    ArpSequencer& getArpSequencer() { return mArpSequencer; }
    const ArpSequencer& getArpSequencer() const { return mArpSequencer; }

    AudioLooper& getAudioLooper() { return mAudioLooper; }
    const AudioLooper& getAudioLooper() const { return mAudioLooper; }

    /**
     * @brief Dispatcher for looper state-change events (push-based).
     *        The audio thread pushes onto its lock-free queue; a worker
     *        thread drains and invokes the registered sink (set by JNI).
     */
    wm::LooperEventDispatcher& getLooperEventDispatcher() {
        return mLooperEventDispatcher;
    }

    /** Musical transport (BPM, beats, metronome scheduler). */
    Transport& getTransport() { return mTransport; }
    const Transport& getTransport() const { return mTransport; }

    /** Recent-output ring used to seed pre-roll on startRecording. */
    PreRollRing& getPreRollRing() { return mPreRollRing; }
    const PreRollRing& getPreRollRing() const { return mPreRollRing; }

    // ========== VOCODER INTEGRATION ==========

    /**
     * @brief Set vocoder carrier frequency (syncs with XY pad)
     * @param frequency Frequency in Hz (20-2000Hz)
     *
     * Call this whenever the oscillator frequency changes to keep
     * the vocoder's internal carrier in sync with the XY pad control.
     * Lock-free: Safe to call from any thread
     */
    void setVocoderCarrierFrequency(float frequency) {
        mEffectChain.setVocoderCarrierFrequency(frequency);
    }

    /**
     * @brief Configure vocoder carrier source based on app mode
     * @param useInternalCarrier true = internal synth, false = input signal
     *
     * Mode configuration:
     * - OSCILLATOR mode (Chaos Pad): false - oscillator output is carrier, self-vocoding
     * - INPUT_FX mode: true - internal synth is carrier, mic is modulator
     * - MIX mode: true - internal synth controlled by XY, mic is modulator
     *
     * Lock-free: Safe to call from any thread
     */
    void setVocoderCarrierSource(bool useInternalCarrier) {
        mEffectChain.setVocoderCarrierSource(useInternalCarrier);
        incrementStateVersion();
    }

    /**
     * @brief Check if a vocoder effect is present in the chain
     * @return true if vocoder exists, false otherwise
     */
    bool hasVocoderEffect() const {
        return mEffectChain.findVocoderIndex() >= 0;
    }

    /**
     * @brief Configure vocoder modulator source
     * @param useExternalMod true = external mic, false = self-vocoding
     *
     * When in CHAOS_PAD mode with mic available, set to true to use
     * the microphone as the modulator signal.
     */
    void setVocoderModulatorSource(bool useExternalMod) {
        mEffectChain.setVocoderModulatorSource(useExternalMod);
    }

    // ========== DUAL TOUCH METHODS (Fase 1, delegated to DualTouchManager) ==========

    void setDualTouchMode(bool enabled);

    void updateDualTouch(
        float x1, float y1, float freq1, float amp1, float pressure1,
        float x2, float y2, float freq2, float amp2, float pressure2,
        float distance, float angle
    ) {
        mDualTouch.update(x1, y1, freq1, amp1, pressure1,
                          x2, y2, freq2, amp2, pressure2,
                          distance, angle);
    }

    void setDualTouchMixMode(DualTouchMixMode mode) {
        mDualTouch.setMixMode(mode);
        incrementStateVersion();
    }

    void setSecondaryOscillatorType(int typeId);

    bool getDualTouchMode() const {
        return mDualTouch.isEnabled();
    }

    // ========== INPUT NODE INTEGRATION (Full-Duplex Monitoring) ==========

    /**
     * @brief Establece el InputNode para monitoring (pass-through de entrada a salida)
     * @param inputNode shared_ptr al InputNode (puede ser nullptr para desactivar)
     *
     * Thread-safe: Puede llamarse desde cualquier thread
     * Uses shared_ptr for safe lifetime management - audio callback holds
     * a local copy to prevent use-after-free if InputNode is released.
     */
    void setInputNode(std::shared_ptr<InputNode> inputNode);

    /**
     * @brief Establece el sample rate preferido para el output stream
     * @param sampleRate Sample rate en Hz (0 para auto-selección)
     *
     * Debe llamarse ANTES de start() para tener efecto.
     * Útil para sincronizar con el sample rate del input stream en monitoring.
     */
    void setPreferredSampleRate(int sampleRate);

    // ========== MODE SYSTEM (Stage 3) ==========

    /**
     * @brief Habilita o deshabilita el renderizado del oscilador
     * @param enabled true para habilitar, false para deshabilitar
     *
     * En modo INPUT_FX, el oscilador debe estar deshabilitado.
     * En modos CHAOS_PAD y MIX, el oscilador debe estar habilitado.
     * Lock-free: Seguro llamar desde cualquier thread
     */
    void setOscillatorEnabled(bool enabled);

    /**
     * @brief Verifica si el oscilador está habilitado
     */
    bool isOscillatorEnabled() const {
        return mOscillatorEnabled.load(std::memory_order_acquire);
    }

    // ========== MIXER NODE CONTROL (Phase 3.1) ==========
    // NOTE: All setters call incrementStateVersion() for future-proof state synchronization

    /**
     * @brief Set crossfade position (0.0 = oscillator only, 1.0 = input only)
     */
    void setMixerCrossfade(float position) {
        if (mMixerNode) {
            mMixerNode->setCrossfade(position);
            incrementStateVersion();
        }
    }

    /**
     * @brief Enable/disable crossfade mode
     */
    void setMixerCrossfadeEnabled(bool enabled) {
        if (mMixerNode) {
            mMixerNode->setCrossfadeEnabled(enabled);
            incrementStateVersion();
        }
    }

    /**
     * @brief Set oscillator level in mixer (0.0 to 2.0)
     */
    void setMixerOscillatorLevel(float level) {
        if (mMixerNode) {
            mMixerNode->setInputLevel(MixerNode::INPUT_OSCILLATOR, level);
            incrementStateVersion();
        }
    }

    /**
     * @brief Set input level in mixer (0.0 to 2.0)
     */
    void setMixerInputLevel(float level) {
        if (mMixerNode) {
            mMixerNode->setInputLevel(MixerNode::INPUT_EXTERNAL, level);
            incrementStateVersion();
        }
    }

    /**
     * @brief Get mixer crossfade position
     */
    float getMixerCrossfade() const {
        return mMixerNode ? mMixerNode->getCrossfade() : 0.5f;
    }

    /**
     * @brief Check if mixer crossfade is enabled
     */
    bool isMixerCrossfadeEnabled() const {
        return mMixerNode ? mMixerNode->isCrossfadeEnabled() : false;
    }

    // ========== EFFECT CHAIN NODE CONTROL (Phase 3.3) ==========

    /**
     * @brief Get the EffectChainNode for direct access (used by XYMapper)
     * @return Pointer to EffectChainNode, or nullptr if not available
     */
    EffectChainNode* getEffectChainNode() {
        return mEffectChainNode.get();
    }

    /**
     * @brief Set global wet/dry mix via EffectChainNode
     */
    void setEffectChainWetDry(float wet) {
        if (mEffectChainNode) {
            mEffectChainNode->setWetDryMix(wet);
            incrementStateVersion();
        }
    }

    /**
     * @brief Get global wet/dry mix from EffectChainNode
     */
    float getEffectChainWetDry() const {
        return mEffectChainNode ? mEffectChainNode->getWetDryMix() : 1.0f;
    }

    /**
     * @brief Get effect count from EffectChainNode
     */
    int getEffectChainNodeCount() const {
        return mEffectChainNode ? mEffectChainNode->getEffectCount() : 0;
    }

    // ========== OUTPUT NODE CONTROL (Phase 4.1) ==========

    /**
     * @brief Get the OutputNode for direct access
     * @return Pointer to OutputNode, or nullptr if not available
     */
    OutputNode* getOutputNode() {
        return mOutputNode.get();
    }

    /**
     * @brief Get peak level from OutputNode (for UI meters)
     * @param channel 0 = left, 1 = right
     */
    float getOutputPeakLevel(int channel) const {
        return mOutputNode ? mOutputNode->getPeakLevel(channel) : 0.0f;
    }

    /**
     * @brief Get RMS level from OutputNode (for UI meters)
     * @param channel 0 = left, 1 = right
     */
    float getOutputRMSLevel(int channel) const {
        return mOutputNode ? mOutputNode->getRMSLevel(channel) : 0.0f;
    }

    /**
     * @brief Enable/disable output limiter
     */
    void setOutputLimiterEnabled(bool enabled) {
        if (mOutputNode) {
            mOutputNode->setLimiterEnabled(enabled);
            incrementStateVersion();
        }
    }

    /**
     * @brief Check if output limiter is enabled
     */
    bool isOutputLimiterEnabled() const {
        return mOutputNode ? mOutputNode->isLimiterEnabled() : true;
    }

    // ========== AUDIO GRAPH CONTROL (Phase 5.2) ==========

    /**
     * @brief Enable/disable AudioGraph processing (feature flag)
     * When enabled, audio processing is delegated to the AudioGraph
     * When disabled, legacy inline processing is used
     */
    void setUseAudioGraph(bool enabled) {
        mUseAudioGraph.store(enabled, std::memory_order_release);
    }

    /**
     * @brief Check if AudioGraph processing is enabled
     */
    bool isUsingAudioGraph() const {
        return mUseAudioGraph.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the AudioGraph for direct access (advanced use)
     */
    AudioGraph* getAudioGraph() {
        return mAudioGraph.get();
    }

    // ========== BACKEND MANAGER CONTROL (USB Audio Phase 1) ==========

    /**
     * @brief Enable/disable BackendManager for audio lifecycle
     *
     * When enabled, start()/stop() delegate to BackendManager
     * which can handle Oboe or USB audio backends.
     *
     * Must be called when engine is stopped.
     *
     * @param enabled true to use BackendManager, false for direct Oboe
     */
    void setUseBackendManager(bool enabled);

    /**
     * @brief Check if BackendManager is being used
     */
    bool isUsingBackendManager() const {
        return mUseBackendManager.load(std::memory_order_acquire);
    }

    // ========== IAUDIOCALLBACK IMPLEMENTATION ==========

    /**
     * @brief Main audio processing callback (backend-agnostic).
     *
     * Called from the audio thread by whichever backend is active (Oboe, USB, etc.)
     * via the IAudioCallback interface.
     *
     * RT-safe: No locks, no allocations, no syscalls.
     */
    watermelon_audio::IAudioCallback::Result onAudioReady(
        float* outputData,
        const float* inputData,
        int32_t numFrames) override;

    /**
     * @brief Called when backend encounters an error
     */
    void onBackendError(watermelon_audio::BackendError error) override;

    /**
     * @brief Called when stream configuration changes
     */
    void onStreamConfigChanged(const watermelon_audio::StreamInfo& newInfo) override;

private:
    /**
     * @brief Core audio processing — called by onAudioReady (IAudioCallback).
     * Contains all DSP: oscillators, voices, effects, looper, output stage.
     * RT-safe: No locks, no allocations, no syscalls.
     */
    watermelon_audio::IAudioCallback::Result processAudioBlock(float* audioData, int32_t numFrames);

    // ========== RENDER SUB-METHODS (Step 8 decomposition) ==========

    /** Handle audio output when engine is not in Running state */
    watermelon_audio::IAudioCallback::Result handleNotRunning(float* output, int32_t numFrames, InputNode* inputNode);

    /** Render via AudioGraph when enabled */
    watermelon_audio::IAudioCallback::Result renderViaGraph(float* output, int32_t numFrames);

    /** Render INPUT_FX mode: input through effect chain */
    void renderInputFx(float* output, int32_t numFrames, InputNode* inputNode);

    /** Render SOUNDFONT mode: SoundFont engine + arp + effects */
    void renderSoundFont(float* output, int32_t numFrames);

    /** Render VOICE_SYSTEM mode: polyphonic voices + effects */
    void renderVoiceSystem(float* output, int32_t numFrames);

    /** Render SINGLE_TOUCH mode: oscillator/engine + arp + effects */
    void renderSingleTouch(float* output, int32_t numFrames,
                           int cachedEngineType, size_t cachedOscIndex,
                           bool cachedHasActiveModulator, size_t cachedModIndex,
                           InputNode* inputNode);

    /** Render DUAL_TOUCH mode: two oscillators + mix + effects */
    void renderDualTouch(float* output, int32_t numFrames,
                         const TouchState& dualTouchState,
                         int cachedEngineType, size_t cachedOscIndex,
                         bool cachedHasActiveModulator, size_t cachedModIndex,
                         InputNode* inputNode);

    /** Handle MIX mode monitoring: mix input with oscillator output */
    void handleMixMonitoring(float* output, int32_t numFrames, InputNode* inputNode,
                             bool oscillatorEnabled, bool hasInputMonitoring);

    /** Common post-processing: DC block + effects + fade + master volume + output protection */
    void applyEffectsAndOutput(float* output, int32_t numFrames);

    /** Pass mic buffer to vocoder modulator if available */
    void feedVocoderModulator(InputNode* inputNode, int32_t numFrames, bool hasInputMonitoring);

    // Legacy Oboe stream (shared_ptr for safe lifetime management)
    // Only used when mUseBackendManager is false (direct Oboe path)
    std::shared_ptr<oboe::AudioStream> mStream;

    // Opaque pointer to Oboe callback adapter (defined in AudioEngine.cpp)
    // Using void* + custom deleter to avoid incomplete type issue with unique_ptr
    struct OboeAdapterDeleter { void operator()(void* p) const; };
    std::unique_ptr<void, OboeAdapterDeleter> mOboeAdapter;

    // Estado del motor (atomic para transiciones thread-safe)
    std::atomic<EngineState> mState{EngineState::Stopped};

    // Mutex para proteger operaciones de start/stop
    std::mutex mStateMutex;

    // Condición para esperar que callbacks terminen
    std::condition_variable mStopCondition;

    // Contador de callbacks activos (para sincronización en stop)
    std::atomic<int> mActiveCallbacks{0};

    // ========== OSCILLATOR BANK (Phase 1E) ==========
    // Owns classic oscillators (primary + dual-touch) and signal modulators
    OscillatorBank mOscBank;

    // Cadena de efectos
    EffectChain mEffectChain;

    // Pending request for EffectChain state reset. Set by UI / JNI
    // thread via requestResetEffectChain(); serviced by the audio
    // thread at the top of onAudioReady() before any effect processing
    // runs for the block. Used on chaos_pad → input_fx transitions to
    // stop stale reverb tails / delay feedback from bleeding into the
    // first blocks of mic processing.
    std::atomic<bool> mResetEffectChainPending{false};

    // ========== OUTPUT STAGE (Phase 1E) ==========
    // DC blocker, limiter, soft clipper, ditherer + scratch buffer
    OutputStage mOutputStage;

    // ========== WAVEFORM CAPTURE (RT-Safe, Phase 1E) ==========
    WaveformCapture mWaveformCapture;  // RT-safe double-buffered waveform

    // Gestión de volumen y fade
    std::atomic<float> mMasterVolume{1.0f};
    FadeController mFadeCtrl;  // Phase 1E: Extracted fade/pause management

    // IMPROVED: Contador de errores en callback (Fase 1.4)
    std::atomic<int> mCallbackErrorCount{0};

    // IMPROVED: Versión del estado para sincronización Kotlin ↔ C++ (Fase 2.1.3)
    std::atomic<uint64_t> mStateVersion{0};

    // IMPROVED: Manejo de errores de stream (Fase 2.1.4)
    std::atomic<bool> mStreamError{false};
    std::atomic<int> mLastStreamErrorCode{0};
    std::atomic<int> mLastXRunCount{0};  // For XRun (underrun/overrun) monitoring
    std::unique_ptr<std::thread> mRecoveryThread;

    // Deferred-stop worker for stopWithFade(). Owned (not detached) so the
    // destructor can join it — a detached thread could outlive the engine and
    // call stop() on freed memory. Touched only from control threads.
    std::unique_ptr<std::thread> mStopFadeThread;
    std::atomic<bool> mStopFadeCancel{false};

    // IMPROVED: Manejo de memoria insuficiente (Fase 2.2.3)
    std::atomic<bool> mInitializationFailed{false};
    bool mUsingReducedBuffers{false};

    // ========== DUAL TOUCH SUPPORT (Phase 1E — delegated to DualTouchManager) ==========
    DualTouchManager mDualTouch;

    // ========== INPUT NODE INTEGRATION (Full-Duplex Monitoring) ==========

    // shared_ptr al InputNode para monitoring (puede ser nullptr)
    // Uses mutex for safe access from UI thread, audio callback makes local copy
    std::shared_ptr<InputNode> mInputNode;
    mutable std::mutex mInputNodeMutex;

    // Buffer pre-alocado para monitoring (RT-safe)
    std::vector<float> mMonitoringBuffer;

    // Sample rate preferido para el output stream (0 = auto)
    std::atomic<int> mPreferredSampleRate{0};

    // ========== MODE SYSTEM (Stage 3) ==========

    // Controla si el oscilador debe renderizar (false en modo INPUT_FX)
    std::atomic<bool> mOscillatorEnabled{true};

    // ========== MIXER NODE INTEGRATION (Phase 3.1) ==========
    // MixerNode for proper level management and crossfade in MIX mode
    std::unique_ptr<MixerNode> mMixerNode;

    // ========== OSCILLATOR NODE (Phase 3.2) ==========
    // OscillatorNode wraps oscillator + modulator system as AudioNode
    // Used for node-based processing (will be primary in AudioGraph)
    std::unique_ptr<OscillatorNode> mOscillatorNode;

    // ========== EFFECT CHAIN NODE (Phase 3.3) ==========
    // EffectChainNode wraps EffectChain as AudioNode with wet/dry mix
    // Provides node-based interface for future AudioGraph integration
    std::unique_ptr<EffectChainNode> mEffectChainNode;

    // ========== OUTPUT NODE (Phase 4.1) ==========
    // OutputNode handles final output protection: DC blocking, soft clip,
    // dithering, master volume, fade, and level metering
    std::unique_ptr<OutputNode> mOutputNode;

    // ========== AUDIO GRAPH (Phase 5.2) ==========
    // AudioGraph manages node-based processing with automatic routing
    // Feature flag allows gradual migration from legacy processing
    std::unique_ptr<AudioGraph> mAudioGraph;
    std::atomic<bool> mUseAudioGraph{false};  // Feature flag for gradual migration

    // ========== BACKEND MANAGER (USB Audio Phase 1) ==========
    // Feature flag to use BackendManager instead of direct Oboe stream
    // When enabled, AudioEngine delegates stream lifecycle to BackendManager.
    //
    // Defaults false on Android to preserve the shipping behaviour (the direct
    // Oboe path), and true everywhere else, where that path does not exist and
    // BackendManager is the only way to open a stream.
#if defined(__ANDROID__)
    std::atomic<bool> mUseBackendManager{false};
#else
    std::atomic<bool> mUseBackendManager{true};
#endif

    // Node handles for quick access to graph nodes
    NodeHandle mGraphOscillatorHandle{INVALID_NODE_HANDLE};
    NodeHandle mGraphMixerHandle{INVALID_NODE_HANDLE};
    NodeHandle mGraphEffectChainHandle{INVALID_NODE_HANDLE};
    NodeHandle mGraphOutputHandle{INVALID_NODE_HANDLE};

    // Pre-allocated AudioBuffers for node-based processing
    AudioBuffer mOscillatorBuffer;  // Oscillator output (non-interleaved)
    AudioBuffer mInputBuffer;       // Input monitoring (non-interleaved)
    AudioBuffer mMixerOutputBuffer; // Mixer output (non-interleaved)
    AudioBuffer mEffectOutputBuffer; // Effect chain output (non-interleaved)

    // ========== GLOBAL BPM (KORG FX) ==========
    std::atomic<float> mBpm{120.0f};           ///< Global BPM for tempo-synced effects
    // El "ultimo BPM propagado" vive en EffectChain::mLastBpm, que es quien de
    // verdad decide si hay que reenviar (EffectChain.cpp:412). La copia que
    // habia aca no se leia ni se escribia desde ningun lado.

    // ========== ARPEGGIATOR (Phase 7) ==========
    ArpSequencer mArpSequencer;
    int mArpSfPrevMidiNote{-1};  // audio thread only — tracks last SoundFont note for clean noteOff

    // ========== AUDIO LOOPER (Phase 11) ==========
    AudioLooper mAudioLooper;
    // Event dispatcher: owned by AudioEngine, lifetime tied to engine.
    // Started in ctor (after wiring into mAudioLooper), stopped+joined in dtor.
    wm::LooperEventDispatcher mLooperEventDispatcher;
    Transport mTransport;
    PreRollRing mPreRollRing;

    // ========== SYNTH ENGINE SYSTEM (Phase 6, Phase 1E — extracted to SynthEngineDispatcher) ==========
    SynthEngineDispatcher mEngineDispatcher;

    // ========== VOICE SYSTEM (Phase 2 - Polyphonic Voices) ==========
    // VoiceManager coordinates trigger sources and voice pool
    // Feature flag allows gradual migration from dual touch
    std::unique_ptr<voice::VoiceManager> mVoiceManager;
    std::atomic<bool> mUseVoiceSystem{false};  // Feature flag for migration

    /**
     * @brief Intenta transicionar a un nuevo estado
     * @return true si la transición fue válida y se realizó
     *
     * Validaciones:
     * - Stopped -> Starting (OK)
     * - Starting -> Running (OK)
     * - Running -> Stopping (OK)
     * - Stopping -> Stopped (OK)
     * - Cualquier otra transición es rechazada
     */
    bool transitionToState(EngineState newState);

    /**
     * Deshace un start que ya había pasado a Running y falló después.
     *
     * Existe porque `Running -> Stopped` NO es una transición válida —desde
     * Running la tabla sólo admite Stopping—, así que el rollback directo se
     * descartaba en silencio y dejaba el motor informando Running sin stream.
     */
    void rollbackFailedStart();

    /**
     * @brief Calcula el volumen actual considerando fade y master volume
     * @return Volumen final a aplicar (0.0 - 1.0)
     */
    float calculateCurrentVolume();

    // processFadeBlock and cancelPendingFade moved to FadeController (Phase 1E)

    /**
     * @brief Incrementa la versión del estado (Fase 2.1.3)
     * Debe llamarse cada vez que cambia el estado del motor
     */
    void incrementStateVersion() {
        mStateVersion.fetch_add(1, std::memory_order_release);
    }

    // mixDualTouchSignals moved to DualTouchManager::mixSignals (Phase 1E)

    /**
     * @brief Configure all audio components with the given sample rate
     * @param sampleRate Sample rate in Hz
     *
     * Called during start() to configure oscillators, modulators, effects,
     * nodes, and other components with the stream's sample rate.
     */
    void configureComponentsWithSampleRate(int sampleRate);

public:
    // ========== GETTERS DE ESTADO PARA JNI ==========

    /**
     * @brief Obtiene el estado actual del motor (para JNI)
     * @return Código de estado: 0=Stopped, 1=Starting, 2=Running, 3=Stopping
     */
    int getEngineState() const {
        return static_cast<int>(mState.load(std::memory_order_acquire));
    }

    /**
     * @brief Verifica si el motor está pausado
     */
    bool getIsPaused() const {
        return mFadeCtrl.isPaused();
    }

    /**
     * @brief Obtiene el volumen maestro actual
     */
    float getMasterVolume() const {
        return mMasterVolume.load(std::memory_order_acquire);
    }

    /**
     * @brief Obtiene el volumen de fade actual
     */
    float getCurrentFadeVolume() const {
        return mFadeCtrl.getCurrentFadeVolume();
    }

    /**
     * @brief Obtiene el volumen target del fade
     */
    float getTargetFadeVolume() const {
        return mFadeCtrl.getTargetFadeVolume();
    }

    /**
     * @brief Verifica si hay un fade en progreso
     */
    bool getIsFading() const {
        return mFadeCtrl.isFading();
    }

    /**
     * @brief Obtiene el progreso del fade (0.0 - 1.0)
     */
    float getFadeProgress() const {
        return mFadeCtrl.getFadeProgress();
    }

    /**
     * @brief Obtiene la versión actual del estado (Fase 2.1.3)
     * @return Contador de versión que se incrementa con cada cambio de estado
     *
     * Usado para detectar desincronización entre Kotlin y C++
     */
    uint64_t getStateVersion() const {
        return mStateVersion.load(std::memory_order_acquire);
    }

    /**
     * @brief Verifica si hay un error de stream activo (Fase 2.1.4)
     */
    bool hasStreamError() const {
        return mStreamError.load(std::memory_order_acquire);
    }

    /**
     * @brief Obtiene el código del último error de stream (Fase 2.1.4)
     * @return Código de error Oboe (0 si no hay error)
     */
    int getLastStreamErrorCode() const {
        return mLastStreamErrorCode.load(std::memory_order_acquire);
    }

    /**
     * @brief Limpia el flag de error de stream (Fase 2.1.4)
     */
    void clearStreamError() {
        mStreamError.store(false, std::memory_order_release);
        mLastStreamErrorCode.store(0, std::memory_order_release);
    }

    /**
     * @brief Obtiene información del stream (thread-safe)
     * @param[out] sampleRate Sample rate del stream (-1 si no hay stream)
     * @param[out] bufferSize Buffer size en frames (-1 si no hay stream)
     * @param[out] latencyMillis Latencia estimada en ms (-1 si no hay stream)
     * @return true si hay un stream activo
     */
    /**
     * @brief Gets stream info (sample rate, buffer size, latency).
     * Works with both legacy Oboe path and BackendManager path.
     */
    bool getStreamInfo(int32_t& sampleRate, int32_t& bufferSize, double& latencyMillis) const;

    /**
     * @brief The sample rate actually in effect, whatever the audio path.
     *
     * Resolves in order: the running stream (BackendManager or legacy Oboe,
     * via getStreamInfo) → the preferred rate → 48000.
     *
     * Use this instead of reaching for mStream directly. Call sites that did
     * `mStream ? mStream->getSampleRate() : 0` silently returned 0 on the
     * BackendManager path, because there mStream is always null — which is how
     * the fade in stopWithFade came to be skipped entirely and how SoundFonts
     * ended up loaded at the *preferred* rate rather than the negotiated one.
     *
     * Never returns <= 0. Not RT-safe (may touch the backend); call from
     * control threads only.
     */
    int currentSampleRate() const;

    /**
     * @brief Gets the legacy Oboe output stream (for benchmark/diagnostics only).
     * @return Pointer to AudioStream or nullptr if using BackendManager.
     * @note Will be removed once legacy Oboe path is eliminated.
     */
    oboe::AudioStream* getOutputStream() const;

    /**
     * @brief Verifica si la inicialización falló completamente (Fase 2.2.3)
     */
    bool hasInitializationFailed() const {
        return mInitializationFailed.load(std::memory_order_acquire);
    }

    /**
     * @brief Verifica si está usando buffers reducidos por memoria baja (Fase 2.2.3)
     */
    bool isUsingReducedBuffers() const {
        return mUsingReducedBuffers;
    }

    // ========== VOICE SYSTEM (Phase 2 - Polyphonic Voices) ==========

    /**
     * @brief Enable or disable the polyphonic voice system
     * @param enable true to use voice system, false for legacy dual touch
     *
     * When enabled, multitouch uses VoiceManager instead of dual oscillators.
     */
    void enableVoiceSystem(bool enable) {
        bool wasEnabled = mUseVoiceSystem.load(std::memory_order_acquire);
        mUseVoiceSystem.store(enable, std::memory_order_release);

        // FIX: When disabling voice system, reset all active voices to
        // prevent hanging notes or residual voice state.
        if (!enable && wasEnabled && mVoiceManager) {
            mVoiceManager->reset();
        }

        incrementStateVersion();
    }

    /**
     * @brief Check if voice system is enabled
     * @return true if voice system is active
     */
    bool isVoiceSystemEnabled() const {
        return mUseVoiceSystem.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the VoiceManager (for advanced configuration)
     * @return Pointer to VoiceManager or nullptr if not available
     */
    voice::VoiceManager* getVoiceManager() {
        return mVoiceManager.get();
    }

    /**
     * @brief Update multitouch state (for voice system)
     * @param touches Array of touch data
     * @param count Number of active touches (0-4)
     *
     * Replaces updateDualTouch when voice system is enabled.
     * Thread-safe: Can be called from UI thread.
     */
    void updateMultiTouch(const voice::TouchData* touches, int count);

    /**
     * @brief Get number of currently active voices
     * @return Active voice count (0 if voice system disabled)
     */
    int getActiveVoiceCount() const {
        if (mVoiceManager) {
            return mVoiceManager->getActiveVoiceCount();
        }
        return 0;
    }

    /**
     * @brief Set maximum number of voices
     * @param max Maximum voices (1-16)
     */
    void setMaxVoices(int max) {
        if (mVoiceManager) {
            mVoiceManager->setMaxVoices(max);
            incrementStateVersion();
        }
    }

    /**
     * @brief Set voice stealing strategy
     * @param strategy Strategy: 0=OLDEST, 1=QUIETEST, 2=SAME_NOTE, 3=LOWEST_PRIORITY
     */
    void setVoiceStealingStrategy(int strategy) {
        if (mVoiceManager && strategy >= 0 && strategy <= 3) {
            mVoiceManager->setStealingStrategy(static_cast<voice::StealingStrategy>(strategy));
            incrementStateVersion();
        }
    }

    // ========== CHORD SYSTEM (Phase 9C) — extracted to ChordHarmony.h ==========
    ChordHarmony mChordHarmony;

    // Delegate methods for backward compatibility (JNI / C API call these)
    void triggerChordNotes(const float* frequencies, int count, float amplitude, int oscillatorType) {
        mChordHarmony.triggerNotes(frequencies, count, amplitude, oscillatorType);
    }
    void updateChordNotes(const float* frequencies, int count, float amplitude) {
        mChordHarmony.updateNotes(frequencies, count, amplitude);
    }
    void releaseChordNotes() {
        mChordHarmony.releaseNotes();
    }
};
