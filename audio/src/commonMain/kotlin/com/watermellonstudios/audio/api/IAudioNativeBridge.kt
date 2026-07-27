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

    // ==================== TRANSPORT (reloj musical + metrónomo) ====================
    //
    // El Transport es dueño del reloj musical: BPM, beats por compás y sample rate,
    // más un scheduler de metrónomo RT-safe. La UI arma una vez y el thread de audio
    // emite los clicks desde el render callback — que es lo que saca el "a veces
    // suena y a veces no" de un timer manejado desde la UI.
    //
    // El BPM NO está acá: se pone con [setBpm] (arriba), que además de al Transport
    // le llega a los efectos sincronizados al tempo. Un setter aparte dejaría a los
    // dos derivando.

    /** Beats por compás, recortado a 1..16. */
    fun transportSetBeatsPerBar(beatsPerBar: Int)
    fun transportGetBeatsPerBar(): Int

    /** Frames por beat al BPM y sample rate actuales. 0 sin motor. */
    fun transportFramesPerBeat(): Int

    /** Frames en `bars` compases completos. Sirve para cuantizar largos de loop. */
    fun transportFramesPerBar(bars: Int): Int

    /**
     * Programa `beats` clicks a intervalo de beat, desde el próximo bloque de audio.
     *
     * @param beats             cantidad de clicks (4 = un compás de cuenta regresiva).
     *                          `<= 0` **detiene** el metrónomo en vez de arrancarlo.
     * @param firstIsDownbeat   el primer click es downbeat (más agudo y fuerte).
     * @param everyBeatPattern  son downbeat los que caen en `index % beatsPerBar == 0`.
     *                          Tiene prioridad sobre [firstIsDownbeat].
     */
    fun transportStartMetronome(
        beats: Int,
        firstIsDownbeat: Boolean = true,
        everyBeatPattern: Boolean = true,
    )

    /** Click continuo hasta [transportStopMetronome] — la referencia mientras se graba. */
    fun transportStartMetronomeContinuous(everyBeatPattern: Boolean = true)

    /** Cancela lo programado. Un click ya sonando decae solo. */
    fun transportStopMetronome()

    fun transportIsMetronomeRunning(): Boolean
    fun transportIsMetronomeContinuous(): Boolean

    /**
     * Clicks que faltan en una cuenta. 0 en reposo.
     *
     * **En modo continuo devuelve el centinela del scheduler (1), no una cuenta de
     * nada** — un schedule continuo no tiene beats restantes. Hay que preguntar
     * [transportIsMetronomeContinuous] antes de leer esto.
     */
    fun transportGetRemainingBeats(): Int

    // ==================== LOOPER (el subconjunto de la tira) ====================
    //
    // **Esto NO es el looper entero.** El JNI tiene 79 funciones; acá hay 11, que
    // son exactamente las que necesita la tira del harness (control 5): preparar en
    // compases, armar, grabar, parar, limpiar, leer estado y exportar.
    //
    // Que sea un subconjunto es deliberado y sigue la regla del opt-in: **algo entra
    // porque un consumidor lo necesita**, y el consumidor de hoy es el harness. Subir
    // las 79 "por completitud" sería fabricar superficie sin caller — justo lo que la
    // decisión de 2026-07-27 quiso evitar. Si NoisyPad pide el looper completo desde
    // commonMain, eso es un ticket con su propia justificación.

    /**
     * Prepara la pista con un largo de `bars` compases al reloj actual.
     *
     * @return frames reservados, o **-1** si `bars` desborda int32 al pasarlo a
     *         frames. Ese -1 es de la tanda 3 de WA-2.6: antes alocaba una pista con
     *         el largo envuelto.
     */
    fun looperPrepareTrackBars(trackIndex: Int, bars: Int, sampleRate: Int): Int

    /**
     * Arma la pista para empezar a grabar en el próximo límite de compás.
     *
     * @return el frame absoluto del disparo (`>= 0`), o **-1 si no se armó nada**.
     *
     * **El -1 hay que mostrarlo.** El bug de la tanda 2 de WA-2.6 era exactamente
     * esto: devolvía un trigger frame para una grabación que nunca arrancaba. Un
     * botón que sólo diga "armado" no lo vuelve a ver.
     */
    fun looperArmAtNextBar(trackIndex: Int): Long

    fun looperStartRecording(trackIndex: Int)
    fun looperStopRecording()
    fun looperStopAll()
    fun looperClearAll()
    fun looperIsRecording(): Boolean
    fun looperIsPlaying(): Boolean
    fun looperIsTrackActive(trackIndex: Int): Boolean
    fun looperIsTrackPlaying(trackIndex: Int): Boolean

    /**
     * Exporta la mezcla a un archivo. **Sincrónico — llamar fuera del main thread.**
     *
     * Devuelve `false` en vez de dejar escapar una excepción (tanda 4 de WA-2.6):
     * antes, un export imposible abortaba el proceso.
     */
    fun looperExportMix(filePath: String): Boolean

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
