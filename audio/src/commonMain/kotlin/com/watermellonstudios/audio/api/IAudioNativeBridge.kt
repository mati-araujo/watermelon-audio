package com.watermellonstudios.audio.api

/**
 * Platform-agnostic interface for the native audio bridge.
 *
 * Extends [IEffectStateProvider] and [IEffectStateWriter] for effect chain operations.
 * Covers lifecycle, state queries, real-time params, voice system, mode, and backend.
 *
 * Platform-specific operations (USB device management, looper, arpeggiator, SoundFont,
 * latency benchmark) are NOT included — their consumers remain platform-specific.
 *
 * Android implementation: [com.watermellonstudios.audio.internal.bridge.AudioNativeBridge]
 */
interface IAudioNativeBridge : IEffectStateProvider, IEffectStateWriter, IInputBridge {

    // ==================== LIFECYCLE ====================

    suspend fun startEngine(): Result<Unit>
    suspend fun stopEngine(): Result<Unit>
    suspend fun startEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun stopEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun pauseEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun resumeEngineWithFade(fadeTimeMs: Int): Result<Unit>

    /** Synchronous lifecycle — for use from AudioEngineImpl (non-suspend context). */
    fun startEngineWithFadeSync(fadeTimeMs: Int)
    fun stopEngineWithFadeSync(fadeTimeMs: Int)
    fun pauseEngineWithFadeSync(fadeTimeMs: Int)
    fun resumeEngineWithFadeSync(fadeTimeMs: Int)
    fun stopEngineSync()

    // ==================== STATE QUERIES ====================

    fun getEngineState(): Int
    fun getStateVersion(): Long
    fun hasStreamError(): Boolean
    fun getLastStreamErrorCode(): Int
    fun clearStreamError()
    fun getIsPaused(): Boolean
    fun isEngineInitialized(): Boolean
    fun hasInitializationFailed(): Boolean
    fun getStreamInfoArray(): FloatArray?
    fun getMasterVolume(): Float

    // ==================== FADE ====================

    fun getCurrentFadeVolume(): Float
    fun getTargetFadeVolume(): Float
    fun getIsFading(): Boolean
    fun getFadeProgress(): Float

    // ==================== REAL-TIME PARAMS (lock-free) ====================

    fun setXY(x: Float, y: Float, coalesce: Boolean = true)
    fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float)
    fun setFrequencyRange(minHz: Float, maxHz: Float)
    fun setMasterVolume(volume: Float)
    fun setOscillatorType(type: Int)
    fun setSecondaryOscillatorType(type: Int)
    fun setEngineType(type: Int)
    fun setEngineParameter(paramId: Int, value: Float)
    fun getEngineType(): Int
    fun setBpm(bpm: Float)
    fun getBpm(): Float
    fun setModulatorType(type: Int)
    fun setModulatorParameter(paramId: Int, value: Float)

    // ==================== EFFECTS (sync variants for AudioEngineImpl) ====================

    fun addEffectSync(typeId: Int): Boolean
    fun removeEffectSync(index: Int)
    fun setEffectParameterSync(effectIndex: Int, paramId: Int, value: Float)
    fun getEffectParameterSync(effectIndex: Int, paramId: Int): Float
    fun setEffectBypassSync(index: Int, bypass: Boolean)
    fun setEffectsBypassSync(bypass: Boolean)
    fun isEffectsBypassedSync(): Boolean
    fun reorderEffectsSync(fromIndex: Int, toIndex: Int)

    // ==================== EFFECT ROUTING ====================

    fun setRoutingMode(mode: Int)
    fun getRoutingMode(): Int
    fun setParallelMix(mix: Float)
    fun setFeedbackAmount(amount: Float)

    // ==================== WAVEFORM ====================

    fun getWaveformSamples(buffer: FloatArray, size: Int): Int

    // ==================== VOICE SYSTEM ====================

    fun enableVoiceSystem(enabled: Boolean)
    fun isVoiceSystemEnabled(): Boolean
    fun updateMultiTouch(count: Int, touchData: FloatArray?)
    fun getActiveVoiceCount(): Int
    fun setMaxVoices(maxVoices: Int)
    fun setVoiceStealingStrategy(strategyId: Int)

    // ==================== DUAL TOUCH ====================

    fun setDualTouchMode(enabled: Boolean)
    fun setDualTouch(
        x1: Float, y1: Float, freq1: Float, amp1: Float, pressure1: Float,
        x2: Float, y2: Float, freq2: Float, amp2: Float, pressure2: Float,
        distance: Float, angle: Float
    )
    fun setDualTouchMixMode(modeId: Int)

    // ==================== CHORD VOICES ====================

    /**
     * Dispara las voces de un acorde. Las voces se asignan en el VoicePool
     * con un SOURCE_ID dedicado a chords (no interfiere con el oscilador principal).
     *
     * @param frequencies Frecuencias de armonía en Hz (NO incluye la raíz)
     * @param amplitude Amplitud 0.0–1.0
     * @param oscillatorType Tipo de oscilador para las voces del acorde
     */
    fun triggerChordNotes(frequencies: FloatArray, amplitude: Float, oscillatorType: Int)

    /** Actualiza freqs y amplitud de las voces activas del acorde (RT-safe). */
    fun updateChordNotes(frequencies: FloatArray, amplitude: Float)

    /** Libera todas las voces del acorde. */
    fun releaseChordNotes()

    // ==================== MODE ====================

    suspend fun setAudioMode(mode: Int): Result<Unit>
    fun getAudioMode(): Int
    fun isInModeTransition(): Boolean

    // ==================== BACKEND ====================

    fun setUseBackendManager(useBackendManager: Boolean)
    fun createSplitBackend(inputBackendId: Int, outputBackendId: Int): Boolean
    fun selectBackend(backendId: Int): Boolean
    fun getCurrentBackendType(): Int
    fun isUsbBackendAvailable(): Boolean

    /**
     * Select the USB latency profile (Fase 1). Re-parametrizes the USB
     * transfer pipeline (iso transfer duration, URBs in flight, pacer jitter
     * budget, DSP block size, ring capacity).
     *
     * Latched and applied on the next USB stream start — same semantics as the
     * USB streaming mode. A running stream keeps its current profile until it is
     * restarted. Fails only if there is no USB backend.
     */
    fun setUsbLatencyProfile(
        profile: com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
    ): Result<Unit>

    // ==================== LOG CAPTURE ====================

    /**
     * Captura de logs del motor — un anillo de 4000 líneas que se lee tirando, no
     * empujando.
     *
     * Es distinto de `AudioLogger`, que es push y entrega cada línea en el momento.
     * Esto existe para que una UI pueda mostrar las últimas N líneas **cuando el
     * usuario las pide**, sin dejar un callback vivo todo el tiempo. Deshabilitada
     * cuesta un load relajado.
     *
     * Global al proceso, no por motor: la C API detrás (`wma_log_capture_*`) no
     * toma `WmaEngine*`, porque el logger tampoco.
     */
    fun setLogCaptureEnabled(enabled: Boolean)

    /**
     * Vacía lo capturado desde la llamada anterior, en formato `"L/TAG: mensaje"`.
     *
     * **Es destructivo**: las líneas se van del anillo. Devuelve un array propio y
     * no un handle a propósito — la C API entrega un `WmaLogBatch*` que hay que
     * liberar, y dejar ese handle cruzar a Kotlin sería regalar un leak a cada
     * llamador. El batch nace y muere dentro de la implementación.
     *
     * Vacío es una respuesta normal, no un error: quiere decir que no pasó nada
     * desde la última vez.
     */
    fun drainCapturedLogs(): Array<String>

    /**
     * Líneas perdidas por desborde del anillo. **No se resetea al vaciar**, así que
     * es un acumulado: si crece entre dos lecturas, la UI está mirando menos de lo
     * que pasó y conviene que lo diga.
     */
    fun getLogCaptureDropped(): Int
}
