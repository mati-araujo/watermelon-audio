package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.modulator.ModulatorType
import com.watermellonstudios.audio.domain.oscillator.OscillatorType
import com.watermellonstudios.audio.domain.scale.ScaleMode
import com.watermellonstudios.audio.domain.state.AudioState
import com.watermellonstudios.audio.domain.AudioBackendType
import kotlinx.coroutines.flow.StateFlow

/**
 * Main interface for the audio engine.
 *
 * This is the primary entry point for all audio operations.
 * Thread-safe and lifecycle-aware.
 *
 * Usage:
 * ```kotlin
 * val engine = AudioEngineFactory.create(context, config)
 *
 * // Start audio
 * engine.start()
 *
 * // Control oscillator
 * engine.setXY(0.5f, 0.7f)
 * engine.setOscillator(OscillatorType.SAW)
 *
 * // Add effects
 * engine.addEffect(EffectType.REVERB)
 *
 * // Observe state
 * engine.state.collect { state ->
 *     updateUI(state)
 * }
 *
 * // Cleanup
 * engine.release()
 * ```
 */
interface AudioEngine {

    // ==================== STATE ====================

    /**
     * Current state of the audio engine.
     * Collect this flow to observe state changes.
     */
    val state: StateFlow<AudioState>

    /**
     * Whether the engine is currently running.
     */
    val isRunning: Boolean

    /**
     * Whether the engine is paused.
     */
    val isPaused: Boolean

    // ==================== LIFECYCLE ====================
    //
    // 🔴 LAS CUATRO DEVUELVEN `Result<Unit>` DESDE REQ-045 (R-API-62), y el cambio es
    // el punto: antes devolvian `Unit` y publicaban `RUNNING` en [state] aunque el
    // motor nativo hubiera dicho que no. O sea que "arranco" era una afirmacion que la
    // libreria no tenia como sostener — el defecto W1 que reporto la auditoria de
    // NoisyPad.
    //
    // **Es compatible en fuente**: un llamador que ignora el retorno sigue
    // compilando igual. El que quiera enterarse ahora puede.
    //
    // Un `success` afirma que el nativo lo hizo. Un `failure` trae la causa TIPADA
    // (`NativeBridgeException`), transportada desde la C API y no re-derivada aca.
    // Las cuatro se serializan entre si bajo el mutex `LIFECYCLE`.

    /**
     * Start the audio engine with optional fade-in.
     *
     * @param fadeMs Fade-in duration in milliseconds (default from config)
     * @return `success` si el motor arranco de verdad; `failure` con la causa si no
     *   —por ejemplo `NativeBridgeException.StreamError` cuando el backend no pudo
     *   abrir el stream. En ese caso [state] **no** pasa a `RUNNING`.
     */
    suspend fun start(fadeMs: Int? = null): Result<Unit>

    /**
     * Stop the audio engine with optional fade-out.
     *
     * @param fadeMs Fade-out duration in milliseconds (default from config)
     * @return `success` si el motor **aceptó** el stop; `failure` con la causa si no.
     *
     * 🔴 **Con `fadeMs > 0` el `success` NO significa "el stream ya está cerrado"** (B9).
     * `AudioEngine::stopWithFade` arranca el fade y delega la detención a un worker que
     * corre cuando el fade termina, así que esta función vuelve antes. `success` afirma
     * exactamente **que el motor aceptó el pedido**; la detención se completa después, de
     * forma asíncrona. Con `fadeMs = 0` el camino es sincrónico y no hay nada pendiente.
     * Quien necesite esperar el cierre mira `state.lifecycle`, que el poller sigue.
     */
    suspend fun stop(fadeMs: Int? = null): Result<Unit>

    /**
     * Pause audio output (keeps stream open).
     *
     * @param fadeMs Fade-out duration in milliseconds
     * @return `success` si el motor pauso; `failure` con la causa si no. En ese caso
     *   `isPaused` **no** pasa a `true`.
     */
    suspend fun pause(fadeMs: Int = 300): Result<Unit>

    /**
     * Resume audio output from pause.
     *
     * @param fadeMs Fade-in duration in milliseconds
     * @return `success` si el motor reanudo; `failure` con la causa si no. En ese caso
     *   `isPaused` **no** pasa a `false`.
     */
    suspend fun resume(fadeMs: Int = 300): Result<Unit>

    // ==================== OSCILLATOR ====================

    /**
     * Set the oscillator type.
     */
    fun setOscillator(type: OscillatorType)

    /**
     * Update XY position (0.0 to 1.0 range).
     * X typically maps to frequency, Y to amplitude.
     *
     * @param x Horizontal position (0.0 - 1.0)
     * @param y Vertical position (0.0 - 1.0)
     */
    fun setXY(x: Float, y: Float)

    /**
     * Set frequency and amplitude directly.
     * Use this for precise control or scale quantization.
     *
     * @param frequency Frequency in Hz (20 - 20000)
     * @param amplitude Amplitude (0.0 - 1.0)
     */
    fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float)

    // ==================== MODULATOR ====================

    /**
     * Set the modulator type.
     */
    fun setModulator(type: ModulatorType)

    /**
     * Set a modulator parameter.
     *
     * @param paramId Parameter ID (modulator-specific)
     * @param value Parameter value (typically 0.0 - 1.0)
     */
    fun setModulatorParameter(paramId: Int, value: Float)

    // ==================== EFFECTS ====================

    /**
     * Add an effect to the chain.
     *
     * @param type Effect type to add
     * @return true if added successfully, false if chain is full
     */
    fun addEffect(type: EffectType): Boolean

    /**
     * Remove an effect from the chain.
     *
     * @param index Effect index in chain (0-based)
     */
    fun removeEffect(index: Int)

    /**
     * Set an effect parameter.
     *
     * @param effectIndex Effect index in chain
     * @param paramId Parameter ID (effect-specific)
     * @param value Parameter value
     */
    fun setEffectParameter(effectIndex: Int, paramId: Int, value: Float)

    /**
     * Get an effect parameter value.
     *
     * @param effectIndex Effect index in chain
     * @param paramId Parameter ID
     * @return Parameter value
     */
    fun getEffectParameter(effectIndex: Int, paramId: Int): Float

    /**
     * Set effect bypass state.
     *
     * @param index Effect index in chain
     * @param bypass true to bypass, false to enable
     */
    fun setEffectBypass(index: Int, bypass: Boolean)

    /**
     * Set global effect-chain bypass state.
     *
     * This does not modify individual effect bypass states. It is intended for
     * performance controls such as a guitar FX master bypass.
     *
     * @param bypass true to bypass the whole effect chain, false to process it
     */
    fun setEffectsBypass(bypass: Boolean)

    /**
     * Reorder effects in the chain.
     *
     * @param fromIndex Source index
     * @param toIndex Destination index
     */
    fun reorderEffects(fromIndex: Int, toIndex: Int)

    // ==================== SCALE ====================

    /**
     * Set the musical scale mode for frequency quantization.
     */
    fun setScaleMode(mode: ScaleMode)

    // ==================== CHORD VOICES ====================

    /**
     * Dispara las voces de un acorde (path oscilador / VoicePool).
     * Las frecuencias se computan en la capa de aplicación (ver
     * [com.watermellonstudios.audio.internal.util.ChordGenerator]).
     *
     * @param frequencies Frecuencias de armonía en Hz (NO incluye la raíz)
     * @param amplitude Amplitud 0.0–1.0
     * @param oscillatorType ID del oscilador (ver [OscillatorType.id])
     */
    fun triggerChord(frequencies: FloatArray, amplitude: Float, oscillatorType: Int)

    /**
     * Actualiza freqs y amplitud de las voces activas del acorde.
     * RT-safe — pensado para invocarse durante drag sobre el XY.
     */
    fun updateChord(frequencies: FloatArray, amplitude: Float)

    /** Libera todas las voces del acorde. */
    fun releaseChord()

    // ==================== VOLUME ====================

    /**
     * Set master volume.
     *
     * @param volume Volume level (0.0 - 1.0)
     */
    fun setMasterVolume(volume: Float)

    // ==================== VISUALIZATION ====================

    /**
     * Get waveform samples for visualization.
     *
     * @param buffer Output buffer
     * @param size Number of samples to retrieve
     * @return Number of samples written
     */
    fun getWaveformSamples(buffer: FloatArray, size: Int): Int

    // ==================== DUAL TOUCH ====================

    /**
     * Enable or disable dual touch mode.
     */
    fun setDualTouchEnabled(enabled: Boolean)

    /**
     * Update dual touch parameters.
     *
     * @param params Dual touch parameters
     */
    fun updateDualTouch(params: DualTouchParams)

    /**
     * Set dual touch mix mode.
     *
     * @param mode Mix mode (0=SUM, 1=AVERAGE, etc.)
     */
    fun setDualTouchMixMode(mode: Int)

    /**
     * Set secondary oscillator for dual touch.
     */
    fun setSecondaryOscillator(type: OscillatorType)

    // ==================== VOICE SYSTEM (Phase 2) ====================

    /**
     * Enable or disable the polyphonic voice system.
     * When enabled, replaces dual touch with up to 8 independent voices.
     *
     * @param enabled true to enable voice system, false to use legacy dual touch
     */
    fun enableVoiceSystem(enabled: Boolean)

    /**
     * Check if voice system is enabled.
     *
     * @return true if voice system is active
     */
    fun isVoiceSystemEnabled(): Boolean

    /**
     * Update multi-touch state for voice system.
     * Each touch point triggers an independent voice.
     *
     * @param touches List of touch points (up to 4)
     */
    fun updateMultiTouch(touches: List<MultiTouchPoint>)

    /**
     * Get the number of currently active voices.
     *
     * @return Number of voices in ATTACK, SUSTAIN, or RELEASE state
     */
    fun getActiveVoiceCount(): Int

    /**
     * Set maximum number of simultaneous voices.
     *
     * @param maxVoices Maximum voices (1-16, default 8)
     */
    fun setMaxVoices(maxVoices: Int)

    /**
     * Set voice stealing strategy when all voices are in use.
     *
     * @param strategy Stealing strategy
     */
    fun setVoiceStealingStrategy(strategy: VoiceStealingStrategy)

    // ==================== AUDIO BACKEND ====================

    /**
     * Set the audio backend type.
     * Must be called when the engine is stopped.
     *
     * @param type Backend type to use (OBOE or LIBUSB)
     * @return true if backend was switched successfully
     */
    fun setAudioBackend(type: AudioBackendType): Boolean

    /**
     * Get the current audio backend type.
     *
     * @return Current backend type
     */
    fun getAudioBackend(): AudioBackendType

    /**
     * Check if USB audio backend is available.
     * Returns true if a USB audio device is connected and initialized.
     *
     * @return true if USB backend can be used
     */
    fun isUsbBackendAvailable(): Boolean

    // ==================== CLEANUP ====================

    /**
     * Release all resources.
     * Call this when the engine is no longer needed.
     */
    fun release()
}

/**
 * Parameters for dual touch mode.
 */
data class DualTouchParams(
    val x1: Float,
    val y1: Float,
    val freq1: Float,
    val amp1: Float,
    val pressure1: Float,
    val x2: Float,
    val y2: Float,
    val freq2: Float,
    val amp2: Float,
    val pressure2: Float,
    val distance: Float,
    val angle: Float
)

/**
 * Single touch point for multi-touch voice system.
 */
data class MultiTouchPoint(
    val x: Float,           // Normalized X position (0.0 - 1.0)
    val y: Float,           // Normalized Y position (0.0 - 1.0)
    val frequency: Float,   // Mapped frequency in Hz
    val amplitude: Float,   // Mapped amplitude (0.0 - 1.0)
    val pressure: Float,    // Touch pressure (0.0 - 1.0)
    val pointerId: Int      // Unique touch pointer ID
)

/**
 * Voice stealing strategy when all voices are in use.
 */
enum class VoiceStealingStrategy(val id: Int) {
    /** Steal the voice that has been playing the longest */
    OLDEST(0),
    /** Steal the voice with the lowest current amplitude */
    QUIETEST(1),
    /** Steal a voice playing the same note (for re-triggering) */
    SAME_NOTE(2),
    /** Steal a voice from a lower priority source */
    LOWEST_PRIORITY(3)
}
