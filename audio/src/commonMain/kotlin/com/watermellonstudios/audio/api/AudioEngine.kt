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
     * @return `success` con el motor nativo **ya en Stopped**; `failure` con la causa si no.
     *
     * 🔴 **`success` significa "el motor paró", también con `fadeMs > 0`** (REQ-050, D11).
     * Hasta 2.21.0 significaba "el motor aceptó el pedido" y la detención llegaba un fade
     * + ~60 ms después. Ahora esto vuelve después del fade, con `state.lifecycle` en
     * `STOPPED`. Si el motor no para dentro de `fadeMs` + 4 s, devuelve
     * `failure(NativeBridgeException.Timeout)` y **no** publica `STOPPED`: el poller sigue al
     * motor. Un segundo `stop` durante el fade no reinicia la rampa.
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
     * Fija el tipo de modulador.
     *
     * 🔴 **Devolvía `Unit` y el rechazo del motor se perdía** (REQ-045, D3): un id que el
     * motor no conoce volvía indistinguible de uno aplicado. Compatible en fuente: ignorar
     * el retorno compila igual.
     *
     * @return `failure` con la causa tipada si el motor no lo aplicó. Con `failure`, el
     *         `state` **no** cambia: publicar un modulador que el motor no tiene sería la
     *         misma mentira una capa más arriba.
     */
    fun setModulator(type: ModulatorType): Result<Unit>

    /**
     * Fija un parámetro del modulador.
     *
     * @param paramId Parameter ID (modulator-specific)
     * @param value Parameter value (typically 0.0 - 1.0)
     * @return `failure` si el id no existe o el valor no es finito (REQ-045, D3).
     */
    fun setModulatorParameter(paramId: Int, value: Float): Result<Unit>

    // ==================== EFFECTS ====================

    /**
     * Add an effect to the chain.
     *
     * @param type Effect type to add
     * @return true if added successfully, false if chain is full
     */
    fun addEffect(type: EffectType): Boolean

    /**
     * Quita un efecto de la cadena.
     *
     * 🔴 **Devolvía `Unit`** y un índice fuera de la cadena era un no-op mudo (REQ-045,
     * D3). Compatible en fuente.
     *
     * @param index Effect index in chain (0-based)
     * @return `failure` con `InvalidEffectIndex` si el índice no existe. Con `failure` la
     *         cadena de `state` queda como estaba.
     */
    fun removeEffect(index: Int): Result<Unit>

    /**
     * Fija un parámetro de efecto.
     *
     * @param effectIndex Effect index in chain
     * @param paramId Parameter ID (effect-specific)
     * @param value Parameter value
     * @return `failure` si el índice no existe o el valor no es finito (REQ-045, D3). Con
     *         `failure` el parámetro de `state` no se actualiza.
     */
    fun setEffectParameter(effectIndex: Int, paramId: Int, value: Float): Result<Unit>

    /**
     * Get an effect parameter value.
     *
     * @param effectIndex Effect index in chain
     * @param paramId Parameter ID
     * @return Parameter value
     */
    fun getEffectParameter(effectIndex: Int, paramId: Int): Float

    /**
     * Fija el bypass de UN efecto.
     *
     * @param index Effect index in chain
     * @param bypass true to bypass, false to enable
     * @return `failure` con `InvalidEffectIndex` si el índice no existe (REQ-045, D3).
     */
    fun setEffectBypass(index: Int, bypass: Boolean): Result<Unit>

    /**
     * Set global effect-chain bypass state.
     *
     * This does not modify individual effect bypass states. It is intended for
     * performance controls such as a guitar FX master bypass.
     *
     * @param bypass true to bypass the whole effect chain, false to process it
     * @return `failure` si el motor no lo aplicó (REQ-045, D3).
     */
    fun setEffectsBypass(bypass: Boolean): Result<Unit>

    /**
     * Reordena la cadena de efectos.
     *
     * @param fromIndex Source index
     * @param toIndex Destination index
     * @return `failure` con `InvalidEffectIndex` si alguno de los dos no existe (REQ-045,
     *         D3). Con `failure` el orden de `state` queda como estaba — y eso importa: la
     *         versión anterior hacía el `removeAt`/`add` sobre la lista **igual**, así que
     *         con un índice inválido tiraba `IndexOutOfBoundsException` después de no haber
     *         hecho nada en el motor.
     */
    fun reorderEffects(fromIndex: Int, toIndex: Int): Result<Unit>

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
     * Cada dedo es una voz con su propia envolvente: ataque de ~5 ms al apoyar y, al levantar,
     * un release exponencial de ~80 ms a −60 dB con la última frecuencia y amplitud del dedo.
     * Si el dedo vuelve durante el release, sube desde donde estaba. Con un solo dedo, la salida
     * es esa voz a ganancia 1 en todos los modos.
     *
     * Modos:
     * - `0` SUM y `1` AVERAGE (default): **desde 2.22.0 son la misma ley, suma a ganancia 1 por
     *   voz**. Antes los dos multiplicaban la suma por 0,5, así que una voz bajaba ~6 dB al
     *   entrar la otra. El pico de la suma lo cuida la protección de salida del motor.
     *   AVERAGE conserva su nombre por compatibilidad.
     * - `2` MAX: max(|voz1|, |voz2|) con el signo de la suma.
     * - `3` CROSSFADE: (1 − d)·voz1 + d·voz2, con d = distancia entre los dedos en [0, 1].
     * - `4` RING: voz1·voz2·0,5.
     * - `5` AMPLITUDE_BALANCED: pesos amp_i / (amp1 + amp2).
     *
     * Los modos 2–5 conservan su ley con los dos dedos y comparten la envolvente.
     *
     * @param mode Mix mode (0=SUM, 1=AVERAGE, 2=MAX, 3=CROSSFADE, 4=RING, 5=AMPLITUDE_BALANCED)
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
